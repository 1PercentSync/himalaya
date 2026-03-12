/**
 * @file renderer.cpp
 * @brief Renderer implementation: GPU data filling, render graph construction, pass execution.
 */

#include <himalaya/app/renderer.h>

#include <himalaya/framework/frame_context.h>
#include <himalaya/framework/imgui_backend.h>
#include <himalaya/framework/scene_data.h>
#include <himalaya/rhi/commands.h>
#include <himalaya/rhi/descriptors.h>
#include <himalaya/rhi/resources.h>
#include <himalaya/rhi/swapchain.h>

#include <algorithm>
#include <array>

#include <GLFW/glfw3.h>
#include <spdlog/spdlog.h>

namespace himalaya::app {
    /** @brief Maximum directional lights the LightBuffer can hold. */
    constexpr uint32_t kMaxDirectionalLights = 4;

    // ---- Init / Destroy ----

    void Renderer::init(rhi::Context &ctx,
                        rhi::Swapchain &swapchain,
                        rhi::ResourceManager &rm,
                        rhi::DescriptorManager &dm,
                        framework::ImGuiBackend &imgui,
                        const std::string &hdr_env_path) {
        ctx_ = &ctx;
        swapchain_ = &swapchain;
        resource_manager_ = &rm;
        descriptor_manager_ = &dm;
        imgui_ = &imgui;

        render_graph_.init(resource_manager_);
        render_graph_.set_reference_resolution(swapchain_->extent);
        register_swapchain_images();

        // HDR color buffer as managed resource (ForwardPass writes, TonemappingPass samples)
        managed_hdr_color_ = render_graph_.create_managed_image("HDR Color", {
                                                                    .size_mode = framework::RGSizeMode::Relative,
                                                                    .width_scale = 1.0f,
                                                                    .height_scale = 1.0f,
                                                                    .width = 0,
                                                                    .height = 0,
                                                                    .format = rhi::Format::R16G16B16A16Sfloat,
                                                                    .usage = rhi::ImageUsage::ColorAttachment |
                                                                             rhi::ImageUsage::Sampled,
                                                                    .sample_count = 1,
                                                                    .mip_levels = 1,
                                                                });

        // Resolved depth buffer (1x): direct render target in 1x mode,
        // MSAA depth resolve target (MAX_BIT) in multi-sample mode.
        // Sampled usage for future screen-space effects (SSAO, Contact Shadows).
        managed_depth_ = render_graph_.create_managed_image("Depth", {
                                                                .size_mode = framework::RGSizeMode::Relative,
                                                                .width_scale = 1.0f,
                                                                .height_scale = 1.0f,
                                                                .width = 0,
                                                                .height = 0,
                                                                .format = rhi::Format::D32Sfloat,
                                                                .usage = rhi::ImageUsage::DepthAttachment |
                                                                         rhi::ImageUsage::Sampled,
                                                                .sample_count = 1,
                                                                .mip_levels = 1,
                                                            });

        // Resolved normal buffer (1x): direct render target in 1x mode,
        // MSAA normal resolve target (AVERAGE) in multi-sample mode.
        // Sampled usage for future screen-space effects (SSAO, Contact Shadows).
        managed_normal_ = render_graph_.create_managed_image("Normal", {
                                                                 .size_mode = framework::RGSizeMode::Relative,
                                                                 .width_scale = 1.0f,
                                                                 .height_scale = 1.0f,
                                                                 .width = 0,
                                                                 .height = 0,
                                                                 .format = rhi::Format::A2B10G10R10UnormPack32,
                                                                 .usage = rhi::ImageUsage::ColorAttachment |
                                                                          rhi::ImageUsage::Sampled,
                                                                 .sample_count = 1,
                                                                 .mip_levels = 1,
                                                             });

        // Fall back to the highest supported sample count if the default isn't available
        while (current_sample_count_ > 1 &&
               !(ctx_->msaa_sample_counts & current_sample_count_)) {
            current_sample_count_ >>= 1;
        }

        // MSAA buffers (only created when sample_count > 1; 1x uses resolved targets directly)
        if (current_sample_count_ > 1) {
            managed_msaa_color_ = render_graph_.create_managed_image("MSAA Color", {
                                                                         .size_mode = framework::RGSizeMode::Relative,
                                                                         .width_scale = 1.0f,
                                                                         .height_scale = 1.0f,
                                                                         .width = 0,
                                                                         .height = 0,
                                                                         .format = rhi::Format::R16G16B16A16Sfloat,
                                                                         .usage = rhi::ImageUsage::ColorAttachment,
                                                                         .sample_count = current_sample_count_,
                                                                         .mip_levels = 1,
                                                                     });

            managed_msaa_depth_ = render_graph_.create_managed_image("MSAA Depth", {
                                                                         .size_mode = framework::RGSizeMode::Relative,
                                                                         .width_scale = 1.0f,
                                                                         .height_scale = 1.0f,
                                                                         .width = 0,
                                                                         .height = 0,
                                                                         .format = rhi::Format::D32Sfloat,
                                                                         .usage = rhi::ImageUsage::DepthAttachment,
                                                                         .sample_count = current_sample_count_,
                                                                         .mip_levels = 1,
                                                                     });

            managed_msaa_normal_ = render_graph_.create_managed_image("MSAA Normal", {
                                                                          .size_mode = framework::RGSizeMode::Relative,
                                                                          .width_scale = 1.0f,
                                                                          .height_scale = 1.0f,
                                                                          .width = 0,
                                                                          .height = 0,
                                                                          .format = rhi::Format::A2B10G10R10UnormPack32,
                                                                          .usage = rhi::ImageUsage::ColorAttachment,
                                                                          .sample_count = current_sample_count_,
                                                                          .mip_levels = 1,
                                                                      });
        }

        shader_compiler_.set_include_path("shaders");

        // --- GlobalUBO buffers (per-frame, CpuToGpu) ---
        constexpr const char *kGlobalUboNames[] = {"Global UBO [Frame 0]", "Global UBO [Frame 1]"};
        static_assert(std::size(kGlobalUboNames) == rhi::kMaxFramesInFlight);
        for (uint32_t i = 0; i < rhi::kMaxFramesInFlight; ++i) {
            global_ubo_buffers_[i] = resource_manager_->create_buffer({
                                                                          .size = sizeof(framework::GlobalUniformData),
                                                                          .usage = rhi::BufferUsage::UniformBuffer,
                                                                          .memory = rhi::MemoryUsage::CpuToGpu,
                                                                      }, kGlobalUboNames[i]);
            descriptor_manager_->write_set0_buffer(
                i, 0, global_ubo_buffers_[i],
                sizeof(framework::GlobalUniformData));
        }

        // --- LightBuffer SSBOs (per-frame, CpuToGpu) ---
        constexpr auto light_buffer_size = static_cast<uint64_t>(kMaxDirectionalLights) *
                                           sizeof(framework::GPUDirectionalLight);
        constexpr const char *kLightBufferNames[] = {"Light SSBO [Frame 0]", "Light SSBO [Frame 1]"};
        static_assert(std::size(kLightBufferNames) == rhi::kMaxFramesInFlight);
        for (uint32_t i = 0; i < rhi::kMaxFramesInFlight; ++i) {
            light_buffers_[i] = resource_manager_->create_buffer({
                                                                     .size = light_buffer_size,
                                                                     .usage = rhi::BufferUsage::StorageBuffer,
                                                                     .memory = rhi::MemoryUsage::CpuToGpu,
                                                                 }, kLightBufferNames[i]);
            descriptor_manager_->write_set0_buffer(
                i, 1, light_buffers_[i], light_buffer_size);
        }

        // --- Default sampler ---
        default_sampler_ = resource_manager_->create_sampler({
                                                                 .mag_filter = rhi::Filter::Linear,
                                                                 .min_filter = rhi::Filter::Linear,
                                                                 .mip_mode = rhi::SamplerMipMode::Linear,
                                                                 .wrap_u = rhi::SamplerWrapMode::Repeat,
                                                                 .wrap_v = rhi::SamplerWrapMode::Repeat,
                                                                 .max_anisotropy =
                                                                 resource_manager_->max_sampler_anisotropy(),
                                                                 .max_lod = VK_LOD_CLAMP_NONE,
                                                             }, "Default Sampler");

        material_system_.init(resource_manager_, descriptor_manager_);

        // --- Default textures (needs immediate scope for staging upload) ---
        ctx_->begin_immediate();
        default_textures_ = framework::create_default_textures(
            *resource_manager_, *descriptor_manager_, default_sampler_);
        ctx_->end_immediate();

        // --- IBL precomputation (own immediate scope inside init) ---
        ibl_.init(*ctx_, *resource_manager_, *descriptor_manager_,
                  shader_compiler_, hdr_env_path);

        // --- Depth + Normal PrePass ---
        depth_prepass_.setup(*ctx_,
                             *resource_manager_,
                             *descriptor_manager_,
                             shader_compiler_,
                             current_sample_count_);

        // --- Forward pass ---
        forward_pass_.setup(*ctx_,
                            *resource_manager_,
                            *descriptor_manager_,
                            shader_compiler_,
                            current_sample_count_);

        // --- Skybox pass ---
        skybox_pass_.setup(*ctx_,
                           *resource_manager_,
                           *descriptor_manager_,
                           shader_compiler_);

        // --- Tonemapping pass ---
        tonemapping_pass_.setup(*ctx_,
                                *resource_manager_,
                                *descriptor_manager_,
                                shader_compiler_,
                                swapchain_->format);

        // --- Set 2 binding 0: hdr_color for TonemappingPass sampling ---
        update_hdr_color_descriptor();
    }

    void Renderer::destroy() {
        material_system_.destroy();
        depth_prepass_.destroy();
        forward_pass_.destroy();
        skybox_pass_.destroy();
        tonemapping_pass_.destroy();
        ibl_.destroy();

        for (const auto ubo: global_ubo_buffers_) {
            resource_manager_->destroy_buffer(ubo);
        }
        for (const auto buf: light_buffers_) {
            resource_manager_->destroy_buffer(buf);
        }

        // Default textures: unregister from bindless, then destroy images
        descriptor_manager_->unregister_texture(default_textures_.white.bindless_index);
        descriptor_manager_->unregister_texture(default_textures_.flat_normal.bindless_index);
        descriptor_manager_->unregister_texture(default_textures_.black.bindless_index);
        resource_manager_->destroy_image(default_textures_.white.image);
        resource_manager_->destroy_image(default_textures_.flat_normal.image);
        resource_manager_->destroy_image(default_textures_.black.image);
        resource_manager_->destroy_sampler(default_sampler_);

        if (managed_msaa_color_.valid())
            render_graph_.destroy_managed_image(managed_msaa_color_);
        if (managed_msaa_depth_.valid())
            render_graph_.destroy_managed_image(managed_msaa_depth_);
        if (managed_msaa_normal_.valid())
            render_graph_.destroy_managed_image(managed_msaa_normal_);
        render_graph_.destroy_managed_image(managed_hdr_color_);
        render_graph_.destroy_managed_image(managed_depth_);
        render_graph_.destroy_managed_image(managed_normal_);
        unregister_swapchain_images();
    }

    // ---- MSAA switching ----

    void Renderer::handle_msaa_change(const uint32_t new_sample_count) {
        if (new_sample_count == current_sample_count_) return;

        // GPU must be idle before destroying pipelines or managed resources.
        vkQueueWaitIdle(ctx_->graphics_queue);

        const uint32_t old = current_sample_count_;
        current_sample_count_ = new_sample_count;

        if (old > 1 && new_sample_count > 1) {
            // Multi-sample → different multi-sample: update existing resource descriptors
            render_graph_.update_managed_desc(managed_msaa_color_, {
                                                  .size_mode = framework::RGSizeMode::Relative,
                                                  .width_scale = 1.0f,
                                                  .height_scale = 1.0f,
                                                  .width = 0,
                                                  .height = 0,
                                                  .format = rhi::Format::R16G16B16A16Sfloat,
                                                  .usage = rhi::ImageUsage::ColorAttachment,
                                                  .sample_count = new_sample_count,
                                                  .mip_levels = 1,
                                              });
            render_graph_.update_managed_desc(managed_msaa_depth_, {
                                                  .size_mode = framework::RGSizeMode::Relative,
                                                  .width_scale = 1.0f,
                                                  .height_scale = 1.0f,
                                                  .width = 0, .height = 0,
                                                  .format = rhi::Format::D32Sfloat,
                                                  .usage = rhi::ImageUsage::DepthAttachment,
                                                  .sample_count = new_sample_count,
                                                  .mip_levels = 1,
                                              });
            render_graph_.update_managed_desc(managed_msaa_normal_, {
                                                  .size_mode = framework::RGSizeMode::Relative,
                                                  .width_scale = 1.0f,
                                                  .height_scale = 1.0f,
                                                  .width = 0, .height = 0,
                                                  .format = rhi::Format::A2B10G10R10UnormPack32,
                                                  .usage = rhi::ImageUsage::ColorAttachment,
                                                  .sample_count = new_sample_count,
                                                  .mip_levels = 1,
                                              });
        } else if (old > 1) {
            // Multi-sample → 1x: destroy MSAA resources
            render_graph_.destroy_managed_image(managed_msaa_color_);
            render_graph_.destroy_managed_image(managed_msaa_depth_);
            render_graph_.destroy_managed_image(managed_msaa_normal_);
            managed_msaa_color_ = {};
            managed_msaa_depth_ = {};
            managed_msaa_normal_ = {};
        } else {
            // 1x → multi-sample: create MSAA resources
            managed_msaa_color_ = render_graph_.create_managed_image("MSAA Color", {
                                                                         .size_mode = framework::RGSizeMode::Relative,
                                                                         .width_scale = 1.0f,
                                                                         .height_scale = 1.0f,
                                                                         .width = 0, .height = 0,
                                                                         .format = rhi::Format::R16G16B16A16Sfloat,
                                                                         .usage = rhi::ImageUsage::ColorAttachment,
                                                                         .sample_count = new_sample_count,
                                                                         .mip_levels = 1,
                                                                     });
            managed_msaa_depth_ = render_graph_.create_managed_image("MSAA Depth", {
                                                                         .size_mode = framework::RGSizeMode::Relative,
                                                                         .width_scale = 1.0f,
                                                                         .height_scale = 1.0f,
                                                                         .width = 0, .height = 0,
                                                                         .format = rhi::Format::D32Sfloat,
                                                                         .usage = rhi::ImageUsage::DepthAttachment,
                                                                         .sample_count = new_sample_count,
                                                                         .mip_levels = 1,
                                                                     });
            managed_msaa_normal_ = render_graph_.create_managed_image("MSAA Normal", {
                                                                          .size_mode = framework::RGSizeMode::Relative,
                                                                          .width_scale = 1.0f,
                                                                          .height_scale = 1.0f,
                                                                          .width = 0, .height = 0,
                                                                          .format = rhi::Format::A2B10G10R10UnormPack32,
                                                                          .usage = rhi::ImageUsage::ColorAttachment,
                                                                          .sample_count = new_sample_count,
                                                                          .mip_levels = 1,
                                                                      });
        }

        // Rebuild pipelines for MSAA-affected passes
        depth_prepass_.on_sample_count_changed(new_sample_count);
        forward_pass_.on_sample_count_changed(new_sample_count);
    }

    uint32_t Renderer::current_sample_count() const {
        return current_sample_count_;
    }

    // ---- Render ----

    void Renderer::render(rhi::CommandBuffer &cmd, const RenderInput &input) {
        // --- Fill GlobalUBO ---
        const auto &ubo_buf = resource_manager_->get_buffer(global_ubo_buffers_[input.frame_index]);

        framework::GlobalUniformData ubo_data{};
        ubo_data.view = input.camera.view;
        ubo_data.projection = input.camera.projection;
        ubo_data.view_projection = input.camera.view_projection;
        ubo_data.inv_view_projection = input.camera.inv_view_projection;
        ubo_data.camera_position_and_exposure = glm::vec4(input.camera.position, input.exposure);
        ubo_data.screen_size = glm::vec2(
            static_cast<float>(swapchain_->extent.width),
            static_cast<float>(swapchain_->extent.height));
        ubo_data.time = static_cast<float>(glfwGetTime());
        ubo_data.ibl_intensity = input.ibl_intensity;
        ubo_data.irradiance_cubemap_index = ibl_.irradiance_cubemap_index().index;
        ubo_data.prefiltered_cubemap_index = ibl_.prefiltered_cubemap_index().index;
        ubo_data.brdf_lut_index = ibl_.brdf_lut_index().index;
        ubo_data.prefiltered_mip_count = ibl_.prefiltered_mip_count();
        ubo_data.skybox_cubemap_index = ibl_.skybox_cubemap_index().index;
        ubo_data.debug_render_mode = input.debug_render_mode;

        const auto light_count = static_cast<uint32_t>(
            std::min(input.lights.size(), static_cast<size_t>(kMaxDirectionalLights)));
        ubo_data.directional_light_count = light_count;
        std::memcpy(ubo_buf.allocation_info.pMappedData, &ubo_data, sizeof(ubo_data));

        // --- Fill LightBuffer ---
        const auto &light_buf = resource_manager_->get_buffer(light_buffers_[input.frame_index]);
        if (light_count > 0) {
            std::array<framework::GPUDirectionalLight, kMaxDirectionalLights> gpu_lights{};
            for (uint32_t i = 0; i < light_count; ++i) {
                gpu_lights[i].direction_and_intensity = glm::vec4(
                    input.lights[i].direction, input.lights[i].intensity);
                gpu_lights[i].color_and_shadow = glm::vec4(
                    input.lights[i].color, input.lights[i].cast_shadows ? 1.0f : 0.0f);
            }
            std::memcpy(light_buf.allocation_info.pMappedData,
                        gpu_lights.data(),
                        light_count * sizeof(framework::GPUDirectionalLight));
        }

        // --- Build render graph ---
        render_graph_.clear();

        const auto swapchain_image = render_graph_.import_image(
            "Swapchain",
            swapchain_image_handles_[input.image_index],
            VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);

        const auto hdr_color_resource = render_graph_.use_managed_image(managed_hdr_color_);
        const auto depth_resource = render_graph_.use_managed_image(managed_depth_);

        const auto normal_resource = render_graph_.use_managed_image(managed_normal_);

        // Use MSAA managed resources when multi-sampled
        framework::RGResourceId msaa_color_resource;
        framework::RGResourceId msaa_depth_resource;
        framework::RGResourceId msaa_normal_resource;
        if (managed_msaa_color_.valid())
            msaa_color_resource = render_graph_.use_managed_image(managed_msaa_color_);
        if (managed_msaa_depth_.valid())
            msaa_depth_resource = render_graph_.use_managed_image(managed_msaa_depth_);
        if (managed_msaa_normal_.valid())
            msaa_normal_resource = render_graph_.use_managed_image(managed_msaa_normal_);

        // --- Construct FrameContext ---
        framework::FrameContext frame_ctx{};
        frame_ctx.swapchain = swapchain_image;
        frame_ctx.hdr_color = hdr_color_resource;
        frame_ctx.depth = depth_resource;
        frame_ctx.normal = normal_resource;
        frame_ctx.msaa_color = msaa_color_resource;
        frame_ctx.msaa_depth = msaa_depth_resource;
        frame_ctx.msaa_normal = msaa_normal_resource;
        frame_ctx.meshes = input.meshes;
        frame_ctx.materials = input.materials;
        frame_ctx.cull_result = &input.cull_result;
        frame_ctx.mesh_instances = input.mesh_instances;
        frame_ctx.frame_index = input.frame_index;
        frame_ctx.sample_count = current_sample_count_;

        // --- Depth + Normal PrePass ---
        depth_prepass_.record(render_graph_, frame_ctx);

        // --- Forward pass ---
        forward_pass_.record(render_graph_, frame_ctx);

        // --- Skybox pass (resolved 1x hdr_color + resolved depth) ---
        skybox_pass_.record(render_graph_, frame_ctx);

        // --- Tonemapping pass ---
        tonemapping_pass_.record(render_graph_, frame_ctx);

        // --- ImGui pass ---
        const std::array imgui_resources = {
            framework::RGResourceUsage{
                swapchain_image,
                framework::RGAccessType::ReadWrite,
                framework::RGStage::ColorAttachment
            },
        };
        render_graph_.add_pass("ImGui", imgui_resources,
                               [this, &input](const rhi::CommandBuffer &pass_cmd) {
                                   VkRenderingAttachmentInfo color_attachment{};
                                   color_attachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
                                   color_attachment.imageView = swapchain_->image_views[input.image_index];
                                   color_attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                                   color_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
                                   color_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

                                   VkRenderingInfo rendering_info{};
                                   rendering_info.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
                                   rendering_info.renderArea = {{0, 0}, swapchain_->extent};
                                   rendering_info.layerCount = 1;
                                   rendering_info.colorAttachmentCount = 1;
                                   rendering_info.pColorAttachments = &color_attachment;

                                   pass_cmd.begin_rendering(rendering_info);
                                   imgui_->render(pass_cmd.handle());
                                   pass_cmd.end_rendering();
                               });

        render_graph_.compile();
        render_graph_.execute(cmd);
    }

    // ---- Resize handling ----

    void Renderer::on_swapchain_invalidated() {
        unregister_swapchain_images();
    }

    void Renderer::on_swapchain_recreated() {
        register_swapchain_images();
        render_graph_.set_reference_resolution(swapchain_->extent);

        // Update Set 2 binding 0 with the resized hdr_color backing image
        update_hdr_color_descriptor();

        // Notify passes of resolution change
        depth_prepass_.on_resize(swapchain_->extent.width, swapchain_->extent.height);
        forward_pass_.on_resize(swapchain_->extent.width, swapchain_->extent.height);
        tonemapping_pass_.on_resize(swapchain_->extent.width, swapchain_->extent.height);
    }

    // ---- Accessors ----

    rhi::SamplerHandle Renderer::default_sampler() const {
        return default_sampler_;
    }

    const framework::DefaultTextures &Renderer::default_textures() const {
        return default_textures_;
    }

    framework::MaterialSystem &Renderer::material_system() {
        return material_system_;
    }

    // ---- HDR color descriptor update ----

    void Renderer::update_hdr_color_descriptor() const {
        const auto hdr_backing = render_graph_.get_managed_backing_image(managed_hdr_color_);
        descriptor_manager_->update_render_target(0, hdr_backing, default_sampler_);
    }

    // ---- Swapchain image registration ----

    // Maps VkFormat → rhi::Format for swapchain image registration.
    // Only handles formats the swapchain actually selects.
    static rhi::Format swapchain_format_to_rhi(const VkFormat format) {
        switch (format) {
            case VK_FORMAT_B8G8R8A8_SRGB: return rhi::Format::B8G8R8A8Srgb;
            case VK_FORMAT_B8G8R8A8_UNORM: return rhi::Format::B8G8R8A8Unorm;
            case VK_FORMAT_R8G8B8A8_SRGB: return rhi::Format::R8G8B8A8Srgb;
            case VK_FORMAT_R8G8B8A8_UNORM: return rhi::Format::R8G8B8A8Unorm;
            default:
                spdlog::error("Unsupported swapchain format for RHI mapping: {}",
                              static_cast<int>(format));
                std::abort();
        }
    }

    void Renderer::register_swapchain_images() {
        const rhi::ImageDesc desc{
            .width = swapchain_->extent.width,
            .height = swapchain_->extent.height,
            .depth = 1,
            .mip_levels = 1,
            .array_layers = 1,
            .sample_count = 1,
            .format = swapchain_format_to_rhi(swapchain_->format),
            .usage = rhi::ImageUsage::ColorAttachment,
        };

        swapchain_image_handles_.reserve(swapchain_->images.size());
        for (size_t i = 0; i < swapchain_->images.size(); ++i) {
            swapchain_image_handles_.push_back(
                resource_manager_->register_external_image(
                    swapchain_->images[i], swapchain_->image_views[i], desc));
        }
    }

    void Renderer::unregister_swapchain_images() {
        for (const auto handle: swapchain_image_handles_) {
            resource_manager_->unregister_external_image(handle);
        }
        swapchain_image_handles_.clear();
    }
} // namespace himalaya::app
