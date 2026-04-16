/**
 * @file renderer_init.cpp
 * @brief Renderer lifecycle: init, destroy, resize, reload, descriptor helpers.
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
    /** @brief Maximum directional lights the LightBuffer can hold. */
    constexpr uint32_t kMaxDirectionalLights = 1;

    /** @brief Maximum instances the InstanceBuffer can hold. */
    constexpr uint32_t kMaxInstances = 65536;

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

        // HDR color buffer as managed resource (ForwardPass writes, TonemappingPass samples,
        // bake preview clears + blits as transfer destination)
        managed_hdr_color_ = render_graph_.create_managed_image("HDR Color", {
                                                                    .size_mode = framework::RGSizeMode::Relative,
                                                                    .width_scale = 1.0f,
                                                                    .height_scale = 1.0f,
                                                                    .width = 0,
                                                                    .height = 0,
                                                                    .format = rhi::Format::R16G16B16A16Sfloat,
                                                                    .usage = rhi::ImageUsage::ColorAttachment |
                                                                             rhi::ImageUsage::Sampled |
                                                                             rhi::ImageUsage::TransferDst,
                                                                    .sample_count = 1,
                                                                    .mip_levels = 1,
                                                                }, false);

        // Resolved depth buffer (1x, temporal): direct render target in 1x mode,
        // MSAA depth resolve target (MAX_BIT) in multi-sample mode.
        // Temporal for AO reprojection (get_history_image returns previous frame depth).
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
                                                            }, true);

        // Resolved normal buffer (1x): direct render target in 1x mode,
        // MSAA normal resolve target (AVERAGE) in multi-sample mode.
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
                                                             }, false);

        // --- Phase 5 AO/Contact Shadow resources ---

        managed_ao_noisy_ = render_graph_.create_managed_image("AO Noisy", {
                                                                   .size_mode = framework::RGSizeMode::Relative,
                                                                   .width_scale = 1.0f,
                                                                   .height_scale = 1.0f,
                                                                   .width = 0,
                                                                   .height = 0,
                                                                   .format = rhi::Format::R8G8B8A8Unorm,
                                                                   .usage = rhi::ImageUsage::Storage |
                                                                            rhi::ImageUsage::Sampled,
                                                                   .sample_count = 1,
                                                                   .mip_levels = 1,
                                                               }, false);

        managed_ao_blurred_ = render_graph_.create_managed_image("AO Blurred", {
                                                                     .size_mode = framework::RGSizeMode::Relative,
                                                                     .width_scale = 1.0f,
                                                                     .height_scale = 1.0f,
                                                                     .width = 0,
                                                                     .height = 0,
                                                                     .format = rhi::Format::R8G8B8A8Unorm,
                                                                     .usage = rhi::ImageUsage::Storage |
                                                                              rhi::ImageUsage::Sampled,
                                                                     .sample_count = 1,
                                                                     .mip_levels = 1,
                                                                 }, false);

        managed_ao_filtered_ = render_graph_.create_managed_image("AO Filtered", {
                                                                      .size_mode = framework::RGSizeMode::Relative,
                                                                      .width_scale = 1.0f,
                                                                      .height_scale = 1.0f,
                                                                      .width = 0,
                                                                      .height = 0,
                                                                      .format = rhi::Format::R8G8B8A8Unorm,
                                                                      .usage = rhi::ImageUsage::Storage |
                                                                               rhi::ImageUsage::Sampled,
                                                                      .sample_count = 1,
                                                                      .mip_levels = 1,
                                                                  }, true);

        managed_contact_shadow_mask_ = render_graph_.create_managed_image("Contact Shadow Mask", {
                                                                              .size_mode =
                                                                              framework::RGSizeMode::Relative,
                                                                              .width_scale = 1.0f,
                                                                              .height_scale = 1.0f,
                                                                              .width = 0,
                                                                              .height = 0,
                                                                              .format = rhi::Format::R8Unorm,
                                                                              .usage = rhi::ImageUsage::Storage |
                                                                                  rhi::ImageUsage::Sampled,
                                                                              .sample_count = 1,
                                                                              .mip_levels = 1,
                                                                          }, false);

        managed_roughness_ = render_graph_.create_managed_image("Roughness", {
                                                                    .size_mode = framework::RGSizeMode::Relative,
                                                                    .width_scale = 1.0f,
                                                                    .height_scale = 1.0f,
                                                                    .width = 0,
                                                                    .height = 0,
                                                                    .format = rhi::Format::R8Unorm,
                                                                    .usage = rhi::ImageUsage::ColorAttachment |
                                                                             rhi::ImageUsage::Sampled,
                                                                    .sample_count = 1,
                                                                    .mip_levels = 1,
                                                                }, false);

        // --- Phase 6 PT resources (only when RT is supported) ---

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
            bake_denoiser_.init();
        }

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
                                                                     }, false);

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
                                                                     }, false);

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
                                                                      }, false);

            managed_msaa_roughness_ = render_graph_.create_managed_image("MSAA Roughness", {
                                                                             .size_mode =
                                                                             framework::RGSizeMode::Relative,
                                                                             .width_scale = 1.0f,
                                                                             .height_scale = 1.0f,
                                                                             .width = 0,
                                                                             .height = 0,
                                                                             .format = rhi::Format::R8Unorm,
                                                                             .usage = rhi::ImageUsage::ColorAttachment,
                                                                             .sample_count = current_sample_count_,
                                                                             .mip_levels = 1,
                                                                         }, false);
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

        // --- InstanceBuffer SSBOs (per-frame, CpuToGpu, fixed upper bound) ---
        constexpr auto instance_buffer_size = static_cast<uint64_t>(kMaxInstances) *
                                              sizeof(framework::GPUInstanceData);
        constexpr const char *kInstanceBufferNames[] = {"Instance SSBO [Frame 0]", "Instance SSBO [Frame 1]"};
        static_assert(std::size(kInstanceBufferNames) == rhi::kMaxFramesInFlight);
        for (uint32_t i = 0; i < rhi::kMaxFramesInFlight; ++i) {
            instance_buffers_[i] = resource_manager_->create_buffer({
                                                                        .size = instance_buffer_size,
                                                                        .usage = rhi::BufferUsage::StorageBuffer,
                                                                        .memory = rhi::MemoryUsage::CpuToGpu,
                                                                    }, kInstanceBufferNames[i]);
            descriptor_manager_->write_set0_buffer(i, 3, instance_buffers_[i], instance_buffer_size);
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

        shadow_comparison_sampler_ = resource_manager_->create_sampler({
                                                                           .mag_filter = rhi::Filter::Linear,
                                                                           .min_filter = rhi::Filter::Linear,
                                                                           .mip_mode = rhi::SamplerMipMode::Nearest,
                                                                           .wrap_u = rhi::SamplerWrapMode::ClampToEdge,
                                                                           .wrap_v = rhi::SamplerWrapMode::ClampToEdge,
                                                                           .max_anisotropy = 0.0f,
                                                                           .max_lod = 0.0f,
                                                                           .compare_enable = true,
                                                                           .compare_op = rhi::CompareOp::GreaterOrEqual,
                                                                       }, "Shadow Comparison Sampler");

        shadow_depth_sampler_ = resource_manager_->create_sampler({
                                                                      .mag_filter = rhi::Filter::Nearest,
                                                                      .min_filter = rhi::Filter::Nearest,
                                                                      .mip_mode = rhi::SamplerMipMode::Nearest,
                                                                      .wrap_u = rhi::SamplerWrapMode::ClampToEdge,
                                                                      .wrap_v = rhi::SamplerWrapMode::ClampToEdge,
                                                                      .max_anisotropy = 0.0f,
                                                                      .max_lod = 0.0f,
                                                                      .compare_enable = false,
                                                                      .compare_op = rhi::CompareOp::Never,
                                                                  }, "Shadow Depth Sampler");

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

        // --- Bake data manager (scan/load/unload baked lighting data) ---
        bake_data_manager_.init(rm, dm, linear_clamp_sampler_, default_sampler_);

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
        shadow_pass_.setup(*ctx_, *resource_manager_, *descriptor_manager_, shader_compiler_);
        depth_prepass_.setup(*ctx_, *resource_manager_, *descriptor_manager_, shader_compiler_, current_sample_count_);
        forward_pass_.setup(*ctx_, *resource_manager_, *descriptor_manager_, shader_compiler_, current_sample_count_);
        skybox_pass_.setup(*ctx_, *resource_manager_, *descriptor_manager_, shader_compiler_);
        tonemapping_pass_.setup(*ctx_, *resource_manager_, *descriptor_manager_, shader_compiler_, swapchain_->format);
        gtao_pass_.setup(*ctx_, *resource_manager_, *descriptor_manager_, shader_compiler_);
        ao_spatial_pass_.setup(*ctx_, *resource_manager_, *descriptor_manager_, shader_compiler_, nearest_clamp_sampler_);
        ao_temporal_pass_.setup(*ctx_, *resource_manager_, *descriptor_manager_, shader_compiler_, nearest_clamp_sampler_);
        contact_shadows_pass_.setup(*ctx_, *resource_manager_, *descriptor_manager_, shader_compiler_);

        if (ctx_->rt_supported) {
            reference_view_pass_.setup(*ctx_, *resource_manager_, *descriptor_manager_,
                                       shader_compiler_, sobol_buffer_, blue_noise_bindless_.index);
            pos_normal_map_pass_.setup(*ctx_, *resource_manager_, *descriptor_manager_,
                                       shader_compiler_);
            lightmap_baker_pass_.setup(*ctx_, *resource_manager_, *descriptor_manager_,
                                       shader_compiler_, sobol_buffer_, blue_noise_bindless_.index);
            probe_baker_pass_.setup(*ctx_, *resource_manager_, *descriptor_manager_,
                                    shader_compiler_, sobol_buffer_, blue_noise_bindless_.index);
        }

        // --- Set 2 initial descriptor writes ---
        update_hdr_color_descriptor();
        update_depth_descriptor();
        update_normal_descriptor();
        update_ao_descriptor();
        update_contact_shadow_descriptor();
        update_shadow_map_descriptor();
        update_shadow_depth_descriptor();
    }

    void Renderer::destroy() {
        bake_data_manager_.destroy();

        // Clean up any in-progress bake state (per-instance images + vectors)
        destroy_bake_instance_images();
        destroy_probe_bake_instance_images();
        bake_instance_indices_.clear();
        bake_lightmap_sizes_.clear();
        bake_lightmap_keys_.clear();
        bake_probe_positions_.clear();
        bake_state_ = framework::BakeState::Idle;

        emissive_light_builder_.destroy();
        scene_as_builder_.destroy();
        as_manager_.destroy();
        if (ctx_->rt_supported) {
            probe_baker_pass_.destroy();
            lightmap_baker_pass_.destroy();
            reference_view_pass_.destroy();
            pos_normal_map_pass_.destroy();
        }
        ibl_.destroy();
        material_system_.destroy();
        shadow_pass_.destroy();
        depth_prepass_.destroy();
        forward_pass_.destroy();
        skybox_pass_.destroy();
        tonemapping_pass_.destroy();
        gtao_pass_.destroy();
        ao_spatial_pass_.destroy();
        ao_temporal_pass_.destroy();
        contact_shadows_pass_.destroy();

        for (const auto ubo: global_ubo_buffers_) {
            resource_manager_->destroy_buffer(ubo);
        }
        for (const auto buf: light_buffers_) {
            resource_manager_->destroy_buffer(buf);
        }
        for (const auto buf: instance_buffers_) {
            resource_manager_->destroy_buffer(buf);
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
        resource_manager_->destroy_sampler(shadow_comparison_sampler_);
        resource_manager_->destroy_sampler(shadow_depth_sampler_);
        resource_manager_->destroy_sampler(nearest_clamp_sampler_);
        resource_manager_->destroy_sampler(linear_clamp_sampler_);

        if (managed_msaa_color_.valid()) {
            render_graph_.destroy_managed_image(managed_msaa_color_);
        }
        if (managed_msaa_depth_.valid()) {
            render_graph_.destroy_managed_image(managed_msaa_depth_);
        }
        if (managed_msaa_normal_.valid()) {
            render_graph_.destroy_managed_image(managed_msaa_normal_);
        }
        if (managed_msaa_roughness_.valid()) {
            render_graph_.destroy_managed_image(managed_msaa_roughness_);
        }
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
        bake_denoiser_.destroy();
        render_graph_.destroy_managed_image(managed_ao_noisy_);
        render_graph_.destroy_managed_image(managed_ao_blurred_);
        render_graph_.destroy_managed_image(managed_ao_filtered_);
        render_graph_.destroy_managed_image(managed_contact_shadow_mask_);
        render_graph_.destroy_managed_image(managed_hdr_color_);
        render_graph_.destroy_managed_image(managed_depth_);
        render_graph_.destroy_managed_image(managed_normal_);
        render_graph_.destroy_managed_image(managed_roughness_);
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
        descriptor_manager_->write_set0_buffer(5, geo_buf, buf_data.desc.size);

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

    // ---- Environment reload ----

    bool Renderer::reload_environment(const std::string &hdr_path) {
        ibl_.destroy();
        const bool ok = ibl_.init(*ctx_, *resource_manager_, *descriptor_manager_, shader_compiler_, hdr_path);

        if (ctx_->rt_supported && ibl_.alias_table_buffer().valid()) {
            const auto &buf = resource_manager_->get_buffer(ibl_.alias_table_buffer());
            descriptor_manager_->write_set0_env_alias_table(ibl_.alias_table_buffer(), buf.desc.size);
        }

        // Environment change invalidates accumulated PT samples
        reset_pt_accumulation();

        return ok;
    }

    // ---- MSAA switching ----

    void Renderer::handle_msaa_change(const uint32_t new_sample_count) {
        if (new_sample_count == current_sample_count_) return;

        vkQueueWaitIdle(ctx_->graphics_queue);

        const uint32_t old = current_sample_count_;
        current_sample_count_ = new_sample_count;

        if (old > 1 && new_sample_count > 1) {
            render_graph_.update_managed_desc(managed_msaa_color_, {
                                                  .size_mode = framework::RGSizeMode::Relative,
                                                  .width_scale = 1.0f, .height_scale = 1.0f,
                                                  .width = 0, .height = 0,
                                                  .format = rhi::Format::R16G16B16A16Sfloat,
                                                  .usage = rhi::ImageUsage::ColorAttachment,
                                                  .sample_count = new_sample_count, .mip_levels = 1,
                                              });
            render_graph_.update_managed_desc(managed_msaa_depth_, {
                                                  .size_mode = framework::RGSizeMode::Relative,
                                                  .width_scale = 1.0f, .height_scale = 1.0f,
                                                  .width = 0, .height = 0,
                                                  .format = rhi::Format::D32Sfloat,
                                                  .usage = rhi::ImageUsage::DepthAttachment,
                                                  .sample_count = new_sample_count, .mip_levels = 1,
                                              });
            render_graph_.update_managed_desc(managed_msaa_normal_, {
                                                  .size_mode = framework::RGSizeMode::Relative,
                                                  .width_scale = 1.0f, .height_scale = 1.0f,
                                                  .width = 0, .height = 0,
                                                  .format = rhi::Format::A2B10G10R10UnormPack32,
                                                  .usage = rhi::ImageUsage::ColorAttachment,
                                                  .sample_count = new_sample_count, .mip_levels = 1,
                                              });
            render_graph_.update_managed_desc(managed_msaa_roughness_, {
                                                  .size_mode = framework::RGSizeMode::Relative,
                                                  .width_scale = 1.0f, .height_scale = 1.0f,
                                                  .width = 0, .height = 0,
                                                  .format = rhi::Format::R8Unorm,
                                                  .usage = rhi::ImageUsage::ColorAttachment,
                                                  .sample_count = new_sample_count, .mip_levels = 1,
                                              });
        } else if (old > 1) {
            render_graph_.destroy_managed_image(managed_msaa_color_);
            render_graph_.destroy_managed_image(managed_msaa_depth_);
            render_graph_.destroy_managed_image(managed_msaa_normal_);
            render_graph_.destroy_managed_image(managed_msaa_roughness_);
            managed_msaa_color_ = {};
            managed_msaa_depth_ = {};
            managed_msaa_normal_ = {};
            managed_msaa_roughness_ = {};
        } else {
            managed_msaa_color_ = render_graph_.create_managed_image("MSAA Color", {
                .size_mode = framework::RGSizeMode::Relative, .width_scale = 1.0f, .height_scale = 1.0f,
                .width = 0, .height = 0, .format = rhi::Format::R16G16B16A16Sfloat,
                .usage = rhi::ImageUsage::ColorAttachment, .sample_count = new_sample_count, .mip_levels = 1,
            }, false);
            managed_msaa_depth_ = render_graph_.create_managed_image("MSAA Depth", {
                .size_mode = framework::RGSizeMode::Relative, .width_scale = 1.0f, .height_scale = 1.0f,
                .width = 0, .height = 0, .format = rhi::Format::D32Sfloat,
                .usage = rhi::ImageUsage::DepthAttachment, .sample_count = new_sample_count, .mip_levels = 1,
            }, false);
            managed_msaa_normal_ = render_graph_.create_managed_image("MSAA Normal", {
                .size_mode = framework::RGSizeMode::Relative, .width_scale = 1.0f, .height_scale = 1.0f,
                .width = 0, .height = 0, .format = rhi::Format::A2B10G10R10UnormPack32,
                .usage = rhi::ImageUsage::ColorAttachment, .sample_count = new_sample_count, .mip_levels = 1,
            }, false);
            managed_msaa_roughness_ = render_graph_.create_managed_image("MSAA Roughness", {
                .size_mode = framework::RGSizeMode::Relative, .width_scale = 1.0f, .height_scale = 1.0f,
                .width = 0, .height = 0, .format = rhi::Format::R8Unorm,
                .usage = rhi::ImageUsage::ColorAttachment, .sample_count = new_sample_count, .mip_levels = 1,
            }, false);
        }

        depth_prepass_.on_sample_count_changed(new_sample_count);
        forward_pass_.on_sample_count_changed(new_sample_count);
    }

    // ---- Shadow resolution change ----

    void Renderer::handle_shadow_resolution_changed(const uint32_t new_resolution) {
        if (new_resolution == shadow_pass_.resolution()) return;

        vkQueueWaitIdle(ctx_->graphics_queue);
        shadow_pass_.on_resolution_changed(new_resolution);
        update_shadow_map_descriptor();
        update_shadow_depth_descriptor();
    }

    // ---- Shader hot-reload ----

    void Renderer::reload_shaders() {
        vkQueueWaitIdle(ctx_->graphics_queue);

        shadow_pass_.rebuild_pipelines();
        depth_prepass_.rebuild_pipelines();
        forward_pass_.rebuild_pipelines();
        skybox_pass_.rebuild_pipelines();
        tonemapping_pass_.rebuild_pipelines();
        gtao_pass_.rebuild_pipelines();
        ao_spatial_pass_.rebuild_pipelines();
        ao_temporal_pass_.rebuild_pipelines();
        contact_shadows_pass_.rebuild_pipelines();
        if (ctx_->rt_supported) {
            reference_view_pass_.rebuild_pipelines();
            pos_normal_map_pass_.rebuild_pipelines();
            lightmap_baker_pass_.rebuild_pipelines();
            probe_baker_pass_.rebuild_pipelines();
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

        update_hdr_color_descriptor();
        update_depth_descriptor();
        update_normal_descriptor();
        update_ao_descriptor();
        update_contact_shadow_descriptor();

        if (ctx_->rt_supported) {
            denoiser_.on_resize(*resource_manager_,
                                swapchain_->extent.width, swapchain_->extent.height);
            reset_pt_accumulation();
        }
    }

    // ---- Descriptor update helpers ----

    void Renderer::update_hdr_color_descriptor() const {
        const auto hdr_backing = render_graph_.get_managed_backing_image(managed_hdr_color_);
        descriptor_manager_->update_render_target(0, hdr_backing, default_sampler_);
    }

    void Renderer::update_depth_descriptor() const {
        const auto depth_backing = render_graph_.get_managed_backing_image(managed_depth_);
        descriptor_manager_->update_render_target(1, depth_backing, nearest_clamp_sampler_);
    }

    void Renderer::update_normal_descriptor() const {
        const auto normal_backing = render_graph_.get_managed_backing_image(managed_normal_);
        descriptor_manager_->update_render_target(2, normal_backing, nearest_clamp_sampler_);
    }

    void Renderer::update_ao_descriptor() const {
        const auto ao_backing = render_graph_.get_managed_backing_image(managed_ao_filtered_);
        descriptor_manager_->update_render_target(3, ao_backing, linear_clamp_sampler_);
    }

    void Renderer::update_contact_shadow_descriptor() const {
        const auto cs_backing = render_graph_.get_managed_backing_image(managed_contact_shadow_mask_);
        descriptor_manager_->update_render_target(4, cs_backing, linear_clamp_sampler_);
    }

    void Renderer::update_shadow_map_descriptor() const {
        descriptor_manager_->update_render_target(5,
                                                  shadow_pass_.shadow_map_image(),
                                                  shadow_comparison_sampler_);
    }

    void Renderer::update_shadow_depth_descriptor() const {
        descriptor_manager_->update_render_target(6,
                                                  shadow_pass_.shadow_map_image(),
                                                  shadow_depth_sampler_);
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
