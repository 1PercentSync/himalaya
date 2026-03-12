/**
 * @file renderer.cpp
 * @brief Renderer implementation: GPU data filling, render graph construction, pass execution.
 */

#include <himalaya/app/renderer.h>

#include <himalaya/framework/imgui_backend.h>
#include <himalaya/framework/mesh.h>
#include <himalaya/framework/scene_data.h>
#include <himalaya/rhi/commands.h>
#include <himalaya/rhi/descriptors.h>
#include <himalaya/rhi/resources.h>
#include <himalaya/rhi/swapchain.h>

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
                        framework::ImGuiBackend &imgui) {
        ctx_ = &ctx;
        swapchain_ = &swapchain;
        resource_manager_ = &rm;
        descriptor_manager_ = &dm;
        imgui_ = &imgui;

        render_graph_.init(resource_manager_);
        render_graph_.set_reference_resolution(swapchain_->extent);
        register_swapchain_images();

        // Depth buffer as managed resource (auto-rebuilt on resize)
        managed_depth_ = render_graph_.create_managed_image("Depth", {
            .size_mode = framework::RGSizeMode::Relative,
            .width_scale = 1.0f,
            .height_scale = 1.0f,
            .width = 0,
            .height = 0,
            .format = rhi::Format::D32Sfloat,
            .usage = rhi::ImageUsage::DepthAttachment,
            .sample_count = 1,
            .mip_levels = 1,
        });

        shader_compiler_.set_include_path("shaders");

        // --- GlobalUBO buffers (per-frame, CpuToGpu) ---
        for (uint32_t i = 0; i < rhi::kMaxFramesInFlight; ++i) {
            global_ubo_buffers_[i] = resource_manager_->create_buffer({
                .size = sizeof(framework::GlobalUniformData),
                .usage = rhi::BufferUsage::UniformBuffer,
                .memory = rhi::MemoryUsage::CpuToGpu,
            });
            descriptor_manager_->write_set0_buffer(
                i, 0, global_ubo_buffers_[i],
                sizeof(framework::GlobalUniformData));
        }

        // --- LightBuffer SSBOs (per-frame, CpuToGpu) ---
        constexpr auto light_buffer_size = static_cast<uint64_t>(kMaxDirectionalLights) *
                                           sizeof(framework::GPUDirectionalLight);
        for (uint32_t i = 0; i < rhi::kMaxFramesInFlight; ++i) {
            light_buffers_[i] = resource_manager_->create_buffer({
                .size = light_buffer_size,
                .usage = rhi::BufferUsage::StorageBuffer,
                .memory = rhi::MemoryUsage::CpuToGpu,
            });
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
            .max_anisotropy = resource_manager_->max_sampler_anisotropy(),
            .max_lod = VK_LOD_CLAMP_NONE,
        });

        material_system_.init(resource_manager_, descriptor_manager_);

        // --- Default textures (needs immediate scope for staging upload) ---
        ctx_->begin_immediate();
        default_textures_ = framework::create_default_textures(
            *resource_manager_, *descriptor_manager_, default_sampler_);
        ctx_->end_immediate();

        // --- Forward pipeline (forward.vert + forward.frag) ---
        {
            const auto vert_spirv = shader_compiler_.compile_from_file(
                "forward.vert", rhi::ShaderStage::Vertex);
            const auto frag_spirv = shader_compiler_.compile_from_file(
                "forward.frag", rhi::ShaderStage::Fragment);

            // ReSharper disable once CppLocalVariableMayBeConst
            VkShaderModule vert_module = rhi::create_shader_module(ctx_->device, vert_spirv);
            // ReSharper disable once CppLocalVariableMayBeConst
            VkShaderModule frag_module = rhi::create_shader_module(ctx_->device, frag_spirv);

            rhi::GraphicsPipelineDesc desc;
            desc.vertex_shader = vert_module;
            desc.fragment_shader = frag_module;
            desc.color_formats = {swapchain_->format};
            desc.depth_format = VK_FORMAT_D32_SFLOAT;
            desc.sample_count = 1;

            const auto binding = framework::Vertex::binding_description();
            const auto attributes = framework::Vertex::attribute_descriptions();
            desc.vertex_bindings = {binding};
            desc.vertex_attributes = {attributes.begin(), attributes.end()};

            const auto set_layouts = descriptor_manager_->get_global_set_layouts();
            desc.descriptor_set_layouts = {set_layouts[0], set_layouts[1]};

            desc.push_constant_ranges = {
                {
                    .stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                    .offset = 0,
                    .size = sizeof(framework::PushConstantData),
                },
            };

            forward_pipeline_ = rhi::create_graphics_pipeline(ctx_->device, desc);

            vkDestroyShaderModule(ctx_->device, frag_module, nullptr);
            vkDestroyShaderModule(ctx_->device, vert_module, nullptr);
        }
    }

    void Renderer::destroy() {
        material_system_.destroy();
        forward_pipeline_.destroy(ctx_->device);

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

        render_graph_.destroy_managed_image(managed_depth_);
        unregister_swapchain_images();
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
        ubo_data.ambient_intensity = input.ambient_intensity;

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

        const auto depth_resource = render_graph_.use_managed_image(managed_depth_);

        // --- Forward (unlit) pass ---
        const std::array scene_resources = {
            framework::RGResourceUsage{
                swapchain_image,
                framework::RGAccessType::Write,
                framework::RGStage::ColorAttachment
            },
            framework::RGResourceUsage{
                depth_resource,
                framework::RGAccessType::ReadWrite,
                framework::RGStage::DepthAttachment
            },
        };
        render_graph_.add_pass("Forward",
                               scene_resources,
                               [this, &input, depth_resource](const rhi::CommandBuffer &pass_cmd) {
                                   VkRenderingAttachmentInfo color_attachment{};
                                   color_attachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
                                   color_attachment.imageView = swapchain_->image_views[input.image_index];
                                   color_attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                                   color_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
                                   color_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
                                   color_attachment.clearValue.color = {{0.0f, 0.0f, 0.0f, 1.0f}};

                                   VkRenderingAttachmentInfo depth_attachment{};
                                   depth_attachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
                                   const auto depth_handle = render_graph_.get_image(depth_resource);
                                   depth_attachment.imageView = resource_manager_->get_image(depth_handle).view;
                                   depth_attachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
                                   depth_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
                                   depth_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
                                   depth_attachment.clearValue.depthStencil = {0.0f, 0};

                                   VkRenderingInfo rendering_info{};
                                   rendering_info.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
                                   rendering_info.renderArea = {{0, 0}, swapchain_->extent};
                                   rendering_info.layerCount = 1;
                                   rendering_info.colorAttachmentCount = 1;
                                   rendering_info.pColorAttachments = &color_attachment;
                                   rendering_info.pDepthAttachment = &depth_attachment;

                                   pass_cmd.begin_rendering(rendering_info);

                                   pass_cmd.bind_pipeline(forward_pipeline_);

                                   const VkDescriptorSet sets[] = {
                                       descriptor_manager_->get_set0(input.frame_index),
                                       descriptor_manager_->get_set1(),
                                   };
                                   pass_cmd.bind_descriptor_sets(forward_pipeline_.layout, 0, sets, 2);

                                   VkViewport viewport{};
                                   viewport.x = 0.0f;
                                   viewport.y = static_cast<float>(swapchain_->extent.height);
                                   viewport.width = static_cast<float>(swapchain_->extent.width);
                                   viewport.height = -static_cast<float>(swapchain_->extent.height);
                                   viewport.minDepth = 0.0f;
                                   viewport.maxDepth = 1.0f;
                                   pass_cmd.set_viewport(viewport);
                                   pass_cmd.set_scissor({{0, 0}, swapchain_->extent});

                                   pass_cmd.set_front_face(VK_FRONT_FACE_COUNTER_CLOCKWISE);
                                   pass_cmd.set_depth_test_enable(true);
                                   pass_cmd.set_depth_write_enable(true);
                                   pass_cmd.set_depth_compare_op(VK_COMPARE_OP_GREATER);

                                   // Draw visible opaque, then visible transparent (back-to-front)
                                   auto draw_instance = [&](const uint32_t idx) {
                                       const auto &instance = input.mesh_instances[idx];
                                       const auto &mesh = input.meshes[instance.mesh_id];
                                       const auto &material = input.materials[instance.material_id];

                                       pass_cmd.set_cull_mode(
                                           material.double_sided
                                               ? VK_CULL_MODE_NONE
                                               : VK_CULL_MODE_BACK_BIT);

                                       const framework::PushConstantData pc{
                                           .model = instance.transform,
                                           .material_index = material.buffer_offset,
                                       };
                                       pass_cmd.push_constants(
                                           forward_pipeline_.layout,
                                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                                           &pc,
                                           sizeof(pc));

                                       pass_cmd.bind_vertex_buffer(
                                           0,
                                           resource_manager_->get_buffer(mesh.vertex_buffer).buffer);
                                       pass_cmd.bind_index_buffer(
                                           resource_manager_->get_buffer(mesh.index_buffer).buffer,
                                           VK_INDEX_TYPE_UINT32);
                                       pass_cmd.draw_indexed(mesh.index_count);
                                   };

                                   for (const auto idx: input.cull_result.visible_opaque_indices)
                                       draw_instance(idx);
                                   for (const auto idx: input.cull_result.visible_transparent_indices)
                                       draw_instance(idx);

                                   pass_cmd.end_rendering();
                               });

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
