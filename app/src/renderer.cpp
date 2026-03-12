/**
 * @file renderer.cpp
 * @brief Renderer implementation: GPU data filling, render graph construction, pass execution.
 */

#include <himalaya/app/renderer.h>

#include <himalaya/framework/mesh.h>
#include <himalaya/framework/scene_data.h>
#include <himalaya/rhi/descriptors.h>
#include <himalaya/rhi/swapchain.h>
#include <himalaya/rhi/resources.h>

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
        register_swapchain_images();
        create_depth_buffer();

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

            const VkShaderModule vert_module = rhi::create_shader_module(ctx_->device, vert_spirv);
            const VkShaderModule frag_module = rhi::create_shader_module(ctx_->device, frag_spirv);

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

        destroy_depth_buffer();
        unregister_swapchain_images();
    }

    // ---- Render (stub — logic migration in next task) ----

    void Renderer::render(const rhi::CommandBuffer & /*cmd*/,
                          const RenderInput & /*input*/) {
    }

    // ---- Resize handling ----

    void Renderer::on_swapchain_invalidated() {
        destroy_depth_buffer();
        unregister_swapchain_images();
    }

    void Renderer::on_swapchain_recreated() {
        register_swapchain_images();
        create_depth_buffer();
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

    // ---- Depth buffer management ----

    void Renderer::create_depth_buffer() {
        depth_image_ = resource_manager_->create_image({
            .width = swapchain_->extent.width,
            .height = swapchain_->extent.height,
            .depth = 1,
            .mip_levels = 1,
            .sample_count = 1,
            .format = rhi::Format::D32Sfloat,
            .usage = rhi::ImageUsage::DepthAttachment,
        });
    }

    void Renderer::destroy_depth_buffer() {
        if (depth_image_.valid()) {
            resource_manager_->destroy_image(depth_image_);
            depth_image_ = {};
        }
    }
} // namespace himalaya::app
