/**
 * @file renderer_init.cpp
 * @brief Renderer lifecycle: init, destroy, resize, reload.
 */

#include <himalaya/app/renderer.h>
#include <himalaya/app/blue_noise_data.h>
#include <himalaya/app/sobol_direction_data.h>

#include <himalaya/framework/imgui_backend.h>
#include <himalaya/framework/scene_data.h>
#include <himalaya/rhi/descriptors.h>
#include <himalaya/rhi/resources.h>
#include <himalaya/rhi/swapchain.h>

#include <spdlog/spdlog.h>

namespace himalaya::app {
    // ---- Init / Destroy ----

    void Renderer::init(rhi::Context &ctx,
                        rhi::Swapchain &swapchain,
                        rhi::ResourceManager &rm,
                        rhi::DescriptorManager &dm,
                        framework::ImGuiBackend &imgui,
                        const std::string &hdr_path) {
        ctx_ = &ctx;
        swapchain_ = &swapchain;
        resource_manager_ = &rm;
        descriptor_manager_ = &dm;
        imgui_ = &imgui;

        render_graph_.init(resource_manager_);
        render_graph_.set_reference_resolution(swapchain_->extent);
        register_swapchain_images();

        // --- PT resources (only when RT is supported) ---

        if (ctx_->rt_supported) {
            managed_pt_accumulation_ = render_graph_.create_managed_image(
                "PT Accumulation", {
                    .size_mode = framework::RGSizeMode::Relative,
                    .width_scale = 1.0f,
                    .height_scale = 1.0f,
                    .width = 0,
                    .height = 0,
                    .format = rhi::Format::R32G32B32A32Sfloat,
                    .usage = rhi::ImageUsage::Storage | rhi::ImageUsage::Sampled | rhi::ImageUsage::TransferSrc,
                    .sample_count = 1,
                    .mip_levels = 1,
                }, false);
            managed_pt_aux_albedo_ = render_graph_.create_managed_image(
                "PT Aux Albedo", {
                    .size_mode = framework::RGSizeMode::Relative,
                    .width_scale = 1.0f,
                    .height_scale = 1.0f,
                    .width = 0,
                    .height = 0,
                    .format = rhi::Format::R16G16B16A16Sfloat,
                    .usage = rhi::ImageUsage::Storage | rhi::ImageUsage::TransferSrc | rhi::ImageUsage::TransferDst,
                    .sample_count = 1,
                    .mip_levels = 1,
                }, false);
            managed_pt_aux_normal_ = render_graph_.create_managed_image(
                "PT Aux Normal", {
                    .size_mode = framework::RGSizeMode::Relative,
                    .width_scale = 1.0f,
                    .height_scale = 1.0f,
                    .width = 0,
                    .height = 0,
                    .format = rhi::Format::R16G16B16A16Sfloat,
                    .usage = rhi::ImageUsage::Storage | rhi::ImageUsage::TransferSrc | rhi::ImageUsage::TransferDst,
                    .sample_count = 1,
                    .mip_levels = 1,
                }, false);
            managed_denoised_ = render_graph_.create_managed_image(
                "Denoised", {
                    .size_mode = framework::RGSizeMode::Relative,
                    .width_scale = 1.0f,
                    .height_scale = 1.0f,
                    .width = 0,
                    .height = 0,
                    .format = rhi::Format::R32G32B32A32Sfloat,
                    .usage = rhi::ImageUsage::TransferDst | rhi::ImageUsage::Sampled,
                    .sample_count = 1,
                    .mip_levels = 1,
                }, false);

            denoiser_.init(*ctx_, *resource_manager_,
                           swapchain_->extent.width, swapchain_->extent.height);
        }

        shader_compiler_.set_include_path("shaders");
#ifdef NDEBUG
        shader_compiler_.set_cache_category("shader_release");
#else
        shader_compiler_.set_cache_category("shader_debug");
#endif

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
                                                                 .compare_enable = false,
                                                                 .compare_op = rhi::CompareOp::Never,
                                                             }, "Default Sampler");

        material_system_.init(resource_manager_, descriptor_manager_);

        // --- Samplers ---

        nearest_clamp_sampler_ = resource_manager_->create_sampler({
                                                                       .mag_filter = rhi::Filter::Nearest,
                                                                       .min_filter = rhi::Filter::Nearest,
                                                                       .mip_mode = rhi::SamplerMipMode::Nearest,
                                                                       .wrap_u = rhi::SamplerWrapMode::ClampToEdge,
                                                                       .wrap_v = rhi::SamplerWrapMode::ClampToEdge,
                                                                       .max_anisotropy = 0.0f,
                                                                       .max_lod = 0.0f,
                                                                       .compare_enable = false,
                                                                       .compare_op = rhi::CompareOp::Never,
                                                                   }, "Nearest Clamp Sampler");

        linear_clamp_sampler_ = resource_manager_->create_sampler({
                                                                      .mag_filter = rhi::Filter::Linear,
                                                                      .min_filter = rhi::Filter::Linear,
                                                                      .mip_mode = rhi::SamplerMipMode::Nearest,
                                                                      .wrap_u = rhi::SamplerWrapMode::ClampToEdge,
                                                                      .wrap_v = rhi::SamplerWrapMode::ClampToEdge,
                                                                      .max_anisotropy = 0.0f,
                                                                      .max_lod = 0.0f,
                                                                      .compare_enable = false,
                                                                      .compare_op = rhi::CompareOp::Never,
                                                                  }, "Linear Clamp Sampler");

        // --- RT acceleration structure manager (conditional on hardware support) ---
        if (ctx_->rt_supported) {
            as_manager_.init(ctx_);
        }

        // --- Default textures (needs immediate scope for staging upload) ---
        ctx_->begin_immediate();
        default_textures_ = framework::create_default_textures(
            *resource_manager_, *descriptor_manager_, default_sampler_);

        // --- Blue noise texture (128x128 R8Unorm, PT Cranley-Patterson rotation) ---
        blue_noise_image_ = resource_manager_->create_image({
            .width = 128,
            .height = 128,
            .depth = 1,
            .mip_levels = 1,
            .array_layers = 1,
            .sample_count = 1,
            .format = rhi::Format::R8Unorm,
            .usage = rhi::ImageUsage::Sampled | rhi::ImageUsage::TransferDst,
        }, "Blue Noise 128x128");
        resource_manager_->upload_image(blue_noise_image_,
                                        kBlueNoiseData,
                                        sizeof(kBlueNoiseData),
                                        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT);
        blue_noise_bindless_ = descriptor_manager_->register_texture(
            blue_noise_image_, nearest_clamp_sampler_);
        spdlog::info("Blue noise texture registered (bindless={})", blue_noise_bindless_.index);

        // --- Sobol direction number SSBO (128 dims × 32 bits, PT quasi-random sampling) ---
        sobol_buffer_ = resource_manager_->create_buffer({
            .size = sizeof(kSobolDirectionData),
            .usage = rhi::BufferUsage::StorageBuffer | rhi::BufferUsage::TransferDst,
            .memory = rhi::MemoryUsage::GpuOnly,
        }, "Sobol Direction Numbers");
        resource_manager_->upload_buffer(sobol_buffer_,
                                         kSobolDirectionData,
                                         sizeof(kSobolDirectionData));
        spdlog::info("Sobol direction table uploaded ({} bytes)", sizeof(kSobolDirectionData));

        ctx_->end_immediate();

        // --- IBL precomputation ---
        ibl_.init(*ctx_, *resource_manager_, *descriptor_manager_, shader_compiler_, hdr_path);

        if (ctx_->rt_supported && ibl_.alias_table_buffer().valid()) {
            const auto &buf = resource_manager_->get_buffer(ibl_.alias_table_buffer());
            descriptor_manager_->write_set0_env_alias_table(ibl_.alias_table_buffer(), buf.desc.size);
        }

        // --- Pass setup ---
        gaussian_splat_pass_resources_.setup(*ctx_);
        gaussian_splat_reset_pass_.setup(*resource_manager_);
        gaussian_splat_cull_project_pass_.setup(*ctx_,
                                                *descriptor_manager_,
                                                shader_compiler_,
                                                gaussian_splat_pass_resources_.descriptor_set_layout());
        tonemapping_pass_.setup(*ctx_, *resource_manager_, *descriptor_manager_, shader_compiler_, swapchain_->format);

        if (ctx_->rt_supported) {
            reference_view_pass_.setup(*ctx_, *resource_manager_, *descriptor_manager_,
                                       shader_compiler_, sobol_buffer_, blue_noise_bindless_.index);
        }
    }

    void Renderer::destroy() {
        gaussian_splat_scene_builder_.destroy();
        gaussian_splat_reset_pass_.destroy();
        gaussian_splat_cull_project_pass_.destroy();
        gaussian_splat_pass_resources_.destroy();
        emissive_light_builder_.destroy();
        scene_as_builder_.destroy();
        as_manager_.destroy();
        if (ctx_->rt_supported) {
            reference_view_pass_.destroy();
        }
        ibl_.destroy();
        material_system_.destroy();
        tonemapping_pass_.destroy();

        for (const auto ubo: global_ubo_buffers_) {
            resource_manager_->destroy_buffer(ubo);
        }

        descriptor_manager_->unregister_texture(blue_noise_bindless_);
        resource_manager_->destroy_image(blue_noise_image_);
        resource_manager_->destroy_buffer(sobol_buffer_);

        descriptor_manager_->unregister_texture(default_textures_.white.bindless_index);
        descriptor_manager_->unregister_texture(default_textures_.flat_normal.bindless_index);
        descriptor_manager_->unregister_texture(default_textures_.black.bindless_index);
        resource_manager_->destroy_image(default_textures_.white.image);
        resource_manager_->destroy_image(default_textures_.flat_normal.image);
        resource_manager_->destroy_image(default_textures_.black.image);
        resource_manager_->destroy_sampler(default_sampler_);
        resource_manager_->destroy_sampler(nearest_clamp_sampler_);
        resource_manager_->destroy_sampler(linear_clamp_sampler_);

        if (managed_pt_accumulation_.valid()) {
            render_graph_.destroy_managed_image(managed_pt_accumulation_);
        }
        if (managed_pt_aux_albedo_.valid()) {
            render_graph_.destroy_managed_image(managed_pt_aux_albedo_);
        }
        if (managed_pt_aux_normal_.valid()) {
            render_graph_.destroy_managed_image(managed_pt_aux_normal_);
        }
        if (managed_denoised_.valid()) {
            render_graph_.destroy_managed_image(managed_denoised_);
        }
        denoiser_.destroy();
        unregister_swapchain_images();
    }

    // ---- RT scene data (acceleration structures + emissive lights) ----

    void Renderer::build_scene_rt(const std::span<const framework::Mesh> meshes,
                                  const std::span<const framework::MeshInstance> instances,
                                  const std::span<const framework::MaterialInstance> materials,
                                  const std::span<const framework::GPUMaterialData> gpu_materials,
                                  const std::span<const std::vector<framework::Vertex>> mesh_vertices,
                                  const std::span<const std::vector<uint32_t>> mesh_indices) {
        if (!ctx_->rt_supported) {
            return;
        }

        // ---- Acceleration structures (BLAS/TLAS + GeometryInfo) ----

        scene_as_builder_.build(*ctx_, *resource_manager_, as_manager_, meshes, instances, materials);

        // build() may return without creating AS if all primitives are degenerate
        if (scene_as_builder_.tlas_handle().as == VK_NULL_HANDLE) {
            return;
        }

        descriptor_manager_->write_set0_tlas(scene_as_builder_.tlas_handle());

        const auto geo_buf = scene_as_builder_.geometry_info_buffer();
        const auto &buf_data = resource_manager_->get_buffer(geo_buf);
        descriptor_manager_->write_set0_buffer(3, geo_buf, buf_data.desc.size);

        // ---- Emissive light data (triangle buffer + alias table) ----

        emissive_light_builder_.build(*resource_manager_, meshes, instances,
                                      gpu_materials, mesh_vertices, mesh_indices);

        if (emissive_light_builder_.emissive_count() > 0) {
            const auto tri_buf = emissive_light_builder_.triangle_buffer();
            const auto &tri_data = resource_manager_->get_buffer(tri_buf);
            descriptor_manager_->write_set0_emissive_triangles(tri_buf, tri_data.desc.size);

            const auto alias_buf = emissive_light_builder_.alias_table_buffer();
            const auto &alias_data = resource_manager_->get_buffer(alias_buf);
            descriptor_manager_->write_set0_emissive_alias_table(alias_buf, alias_data.desc.size);
        }

        reference_view_pass_.set_emissive_light_count(emissive_light_builder_.emissive_count());
    }

    void Renderer::destroy_scene_rt() {
        if (!ctx_->rt_supported) {
            return;
        }
        scene_as_builder_.destroy();
        emissive_light_builder_.destroy();
        reference_view_pass_.set_emissive_light_count(0);
    }

    // ---- GS scene data ----

    bool Renderer::preflight_gaussian_splat_scene(const framework::GaussianSplatScene &scene,
                                                  std::string &error_message) const {
        return gaussian_splat_scene_builder_.preflight(scene, error_message);
    }

    bool Renderer::build_gaussian_splat_scene(const framework::GaussianSplatScene &scene,
                                              std::string &error_message) {
        return gaussian_splat_scene_builder_.build(*ctx_,
                                                   *resource_manager_,
                                                   gaussian_splat_pass_resources_.descriptor_set_layout(),
                                                   scene,
                                                   error_message);
    }

    void Renderer::destroy_gaussian_splat_scene() {
        gaussian_splat_scene_builder_.destroy();
    }

    // ---- Environment reload ----

    bool Renderer::reload_environment(const std::string &hdr_path) {
        ibl_.destroy();
        const bool ok = ibl_.init(*ctx_, *resource_manager_, *descriptor_manager_, shader_compiler_, hdr_path);

        if (ctx_->rt_supported && ibl_.alias_table_buffer().valid()) {
            const auto &buf = resource_manager_->get_buffer(ibl_.alias_table_buffer());
            descriptor_manager_->write_set0_env_alias_table(ibl_.alias_table_buffer(), buf.desc.size);
        }

        reset_pt_accumulation();

        return ok;
    }

    // ---- Shader hot-reload ----

    void Renderer::reload_shaders() {
        vkQueueWaitIdle(ctx_->graphics_queue);

        tonemapping_pass_.rebuild_pipelines();
        gaussian_splat_cull_project_pass_.rebuild_pipelines();
        if (ctx_->rt_supported) {
            reference_view_pass_.rebuild_pipelines();
        }

        spdlog::info("All shaders reloaded");
    }

    // ---- Resize handling ----

    void Renderer::on_swapchain_invalidated() {
        unregister_swapchain_images();
    }

    void Renderer::on_swapchain_recreated() {
        register_swapchain_images();
        render_graph_.set_reference_resolution(swapchain_->extent);

        if (ctx_->rt_supported) {
            denoiser_.on_resize(*resource_manager_,
                                swapchain_->extent.width, swapchain_->extent.height);
            reset_pt_accumulation();
        }
    }

    // ---- Swapchain image registration ----

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
