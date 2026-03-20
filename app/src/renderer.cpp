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
#include <cmath>

#include <glm/gtc/matrix_transform.hpp>

#include <GLFW/glfw3.h>
#include <spdlog/spdlog.h>

namespace himalaya::app {
    /**
     * @brief Maximum directional lights the LightBuffer can hold.
     *
     * M1 limits to 1 directional light — CSM shadow infrastructure supports
     * only a single shadow-casting light. Multi-directional-light CSM is
     * planned for M2 (multiple shadow maps, per-light cascade arrays).
     */
    constexpr uint32_t kMaxDirectionalLights = 1;

    /**
     * @brief Maximum instances the InstanceBuffer can hold.
     *
     * 65536 × 128 bytes = 8 MB per frame — trivial for modern GPUs.
     * Covers any realistic scene complexity for CPU-driven draw submission.
     * Scenes exceeding this should use GPU-driven indirect rendering (M3+).
     */
    constexpr uint32_t kMaxInstances = 65536;

    // ---- Draw group building helpers ----

    /**
     * Sort predicate for draw group building: orders instance indices by
     * (mesh_id, alpha_mode, double_sided). Shared by camera and shadow paths.
     */
    static auto instance_group_sort(
        const std::span<const framework::MeshInstance> instances,
        const std::span<const framework::MaterialInstance> materials) {
        return [instances, materials](const uint32_t a, const uint32_t b) {
            const auto &ia = instances[a];
            const auto &ib = instances[b];
            if (ia.mesh_id != ib.mesh_id) return ia.mesh_id < ib.mesh_id;
            const auto &ma = materials[ia.material_id];
            const auto &mb = materials[ib.material_id];
            if (ma.alpha_mode != mb.alpha_mode) return ma.alpha_mode < mb.alpha_mode;
            return ma.double_sided < mb.double_sided;
        };
    }

    /**
     * Builds MeshDrawGroup lists from pre-sorted instance indices.
     *
     * Groups consecutive entries sharing (mesh_id, alpha_mode, double_sided),
     * fills GPU instance data into the mapped InstanceBuffer, and routes
     * groups to opaque or mask output vectors.
     *
     * @param compute_normal_matrix  If false, skips the per-instance
     *     mat3 inverse+transpose (shadow shaders don't read normal data).
     */
    static void build_draw_groups(
        const std::span<const framework::MeshInstance> mesh_instances,
        const std::span<const framework::MaterialInstance> materials,
        const std::span<const uint32_t> sorted_indices,
        framework::GPUInstanceData *gpu_instances,
        uint32_t &instance_offset,
        std::vector<framework::MeshDrawGroup> &out_opaque,
        std::vector<framework::MeshDrawGroup> &out_mask,
        const bool compute_normal_matrix) {
        out_opaque.clear();
        out_mask.clear();

        const auto count = static_cast<uint32_t>(sorted_indices.size());
        uint32_t group_start = 0;
        while (group_start < count) {
            const uint32_t first_idx = sorted_indices[group_start];
            const uint32_t mesh_id = mesh_instances[first_idx].mesh_id;
            const auto &material = materials[mesh_instances[first_idx].material_id];

            // Find end of group (same mesh_id + alpha_mode + double_sided)
            uint32_t group_end = group_start + 1;
            while (group_end < count) {
                const auto &inst_e = mesh_instances[sorted_indices[group_end]];
                const auto &mat_e = materials[inst_e.material_id];
                if (inst_e.mesh_id != mesh_id ||
                    mat_e.alpha_mode != material.alpha_mode ||
                    mat_e.double_sided != material.double_sided)
                    break;
                ++group_end;
            }

            const uint32_t group_count = group_end - group_start;

            // Guard against InstanceBuffer overflow
            if (instance_offset + group_count > kMaxInstances) {
                spdlog::warn("InstanceBuffer overflow: {} instances exceed limit {}, "
                             "dropping remaining draw groups",
                             instance_offset + group_count, kMaxInstances);
                break;
            }

            const uint32_t group_first = instance_offset;

            // Fill InstanceBuffer entries for this group
            for (uint32_t i = group_start; i < group_end; ++i) {
                const auto &inst = mesh_instances[sorted_indices[i]];
                framework::GPUInstanceData data{};
                data.model = inst.transform;
                data.material_index = materials[inst.material_id].buffer_offset;
                if (compute_normal_matrix) {
                    const glm::mat3 nm = glm::transpose(glm::inverse(glm::mat3(inst.transform)));
                    data.normal_col0 = glm::vec4(nm[0], 0.0f);
                    data.normal_col1 = glm::vec4(nm[1], 0.0f);
                    data.normal_col2 = glm::vec4(nm[2], 0.0f);
                }
                gpu_instances[instance_offset++] = data;
            }

            // Route to opaque or mask list based on alpha mode
            const framework::MeshDrawGroup group{
                .mesh_id = mesh_id,
                .first_instance = group_first,
                .instance_count = group_count,
                .double_sided = material.double_sided,
            };

            if (material.alpha_mode == framework::AlphaMode::Mask) {
                out_mask.push_back(group);
            } else {
                out_opaque.push_back(group);
            }

            group_start = group_end;
        }
    }

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

        // --- Default textures (needs immediate scope for staging upload) ---
        ctx_->begin_immediate();
        default_textures_ = framework::create_default_textures(
            *resource_manager_, *descriptor_manager_, default_sampler_);
        ctx_->end_immediate();

        // --- IBL precomputation (equirect → cubemap → irradiance/prefiltered/BRDF LUT) ---
        ibl_.init(*ctx_, *resource_manager_, *descriptor_manager_, shader_compiler_, hdr_path);

        // --- Shadow comparison sampler (Reverse-Z: fragment depth >= shadow depth → lit) ---
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

        // --- Shadow pass ---
        shadow_pass_.setup(*ctx_,
                           *resource_manager_,
                           *descriptor_manager_,
                           shader_compiler_);

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

        // --- Set 2 binding 5: shadow map for forward pass shadow sampling ---
        update_shadow_map_descriptor();
    }

    void Renderer::destroy() {
        ibl_.destroy();
        material_system_.destroy();
        shadow_pass_.destroy();
        depth_prepass_.destroy();
        forward_pass_.destroy();
        skybox_pass_.destroy();
        tonemapping_pass_.destroy();

        for (const auto ubo: global_ubo_buffers_) {
            resource_manager_->destroy_buffer(ubo);
        }
        for (const auto buf: light_buffers_) {
            resource_manager_->destroy_buffer(buf);
        }
        for (const auto buf: instance_buffers_) {
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
        resource_manager_->destroy_sampler(shadow_comparison_sampler_);

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

    // ---- Environment reload ----

    bool Renderer::reload_environment(const std::string &hdr_path) {
        // Caller guarantees GPU idle (vkQueueWaitIdle already called).
        ibl_.destroy();
        return ibl_.init(*ctx_, *resource_manager_, *descriptor_manager_, shader_compiler_, hdr_path);
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

    // ---- Shader hot-reload ----

    void Renderer::reload_shaders() {
        vkQueueWaitIdle(ctx_->graphics_queue);

        shadow_pass_.rebuild_pipelines();
        depth_prepass_.rebuild_pipelines();
        forward_pass_.rebuild_pipelines();
        skybox_pass_.rebuild_pipelines();
        tonemapping_pass_.rebuild_pipelines();

        spdlog::info("All shaders reloaded");
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
        ubo_data.ibl_rotation_sin = input.ibl_rotation_sin;
        ubo_data.ibl_rotation_cos = input.ibl_rotation_cos;
        ubo_data.debug_render_mode = input.debug_render_mode;

        // Find the first shadow-casting directional light (used by UBO fill + draw group + pass recording)
        const framework::DirectionalLight *shadow_light = nullptr;
        for (const auto &light : input.lights) {
            if (light.cast_shadows) {
                shadow_light = &light;
                break;
            }
        }
        const bool shadows_active = input.features.shadows && shadow_light;

        // --- Feature flags ---
        ubo_data.feature_flags = 0;
        if (shadows_active) {
            ubo_data.feature_flags |= 1u; // FEATURE_SHADOWS
        }

        // --- Shadow fields ---
        ubo_data.shadow_normal_offset = input.shadow_config.normal_offset;
        ubo_data.shadow_texel_size = 1.0f / static_cast<float>(shadow_pass_.resolution());
        ubo_data.shadow_max_distance = input.shadow_config.max_distance;
        ubo_data.shadow_blend_width = input.shadow_config.blend_width;
        ubo_data.shadow_distance_fade_width = input.shadow_config.distance_fade_width;
        ubo_data.shadow_pcf_radius = input.shadow_config.pcf_radius;

        if (shadows_active) {
            ubo_data.shadow_cascade_count = 1; // Step 2: single cascade

            const auto &cam = input.camera;
            const glm::vec3 light_dir = glm::normalize(shadow_light->direction);
            const float shadow_far = std::min(input.shadow_config.max_distance, cam.far_plane);

            // Compute camera sub-frustum corners (near to shadow_far)
            const glm::vec3 fwd = cam.forward();
            const glm::vec3 rgt = cam.right();
            const glm::vec3 up = glm::cross(rgt, fwd);

            const float tan_half = std::tan(cam.fov * 0.5f);
            const float near_h = cam.near_plane * tan_half;
            const float near_w = near_h * cam.aspect;
            const float far_h = shadow_far * tan_half;
            const float far_w = far_h * cam.aspect;

            const glm::vec3 near_center = cam.position + fwd * cam.near_plane;
            const glm::vec3 far_center = cam.position + fwd * shadow_far;

            const std::array<glm::vec3, 8> corners = {
                near_center - rgt * near_w + up * near_h,
                near_center + rgt * near_w + up * near_h,
                near_center - rgt * near_w - up * near_h,
                near_center + rgt * near_w - up * near_h,
                far_center - rgt * far_w + up * far_h,
                far_center + rgt * far_w + up * far_h,
                far_center - rgt * far_w - up * far_h,
                far_center + rgt * far_w - up * far_h,
            };

            // Light view matrix — center on frustum midpoint
            const glm::vec3 frustum_center = (near_center + far_center) * 0.5f;
            glm::vec3 light_up(0.0f, 1.0f, 0.0f);
            if (std::abs(glm::dot(light_dir, light_up)) > 0.99f) {
                light_up = glm::vec3(0.0f, 0.0f, 1.0f);
            }
            const glm::mat4 light_view = glm::lookAt(
                frustum_center - light_dir, frustum_center, light_up);

            // Find AABB of frustum corners in light space (XY tight fit)
            glm::vec3 ls_min(std::numeric_limits<float>::max());
            glm::vec3 ls_max(std::numeric_limits<float>::lowest());
            for (const auto &c: corners) {
                const auto ls = glm::vec3(light_view * glm::vec4(c, 1.0f));
                ls_min = glm::min(ls_min, ls);
                ls_max = glm::max(ls_max, ls);
            }

            // Extend Z range to include the entire scene AABB so that shadow
            // casters outside the camera frustum (e.g. tall objects above the
            // view) are not clipped by the light projection near/far planes.
            {
                const auto &sb = input.scene_bounds;
                const std::array<glm::vec3, 8> scene_corners = {
                    glm::vec3{sb.min.x, sb.min.y, sb.min.z},
                    glm::vec3{sb.max.x, sb.min.y, sb.min.z},
                    glm::vec3{sb.min.x, sb.max.y, sb.min.z},
                    glm::vec3{sb.max.x, sb.max.y, sb.min.z},
                    glm::vec3{sb.min.x, sb.min.y, sb.max.z},
                    glm::vec3{sb.max.x, sb.min.y, sb.max.z},
                    glm::vec3{sb.min.x, sb.max.y, sb.max.z},
                    glm::vec3{sb.max.x, sb.max.y, sb.max.z},
                };
                for (const auto &c : scene_corners) {
                    const float lz = glm::dot(
                        glm::vec3(light_view[0][2], light_view[1][2], light_view[2][2]), c)
                        + light_view[3][2];
                    ls_min.z = std::min(ls_min.z, lz);
                    ls_max.z = std::max(ls_max.z, lz);
                }
            }

            // Build reverse-Z orthographic projection
            // Standard RH_ZO maps near→0, far→1; reverse: near→1, far→0
            glm::mat4 light_proj = glm::orthoRH_ZO(
                ls_min.x,
                ls_max.x,
                ls_min.y,
                ls_max.y,
                -ls_max.z,
                -ls_min.z);
            light_proj[2][2] = -light_proj[2][2];
            light_proj[3][2] = 1.0f - light_proj[3][2];

            ubo_data.cascade_view_proj[0] = light_proj * light_view;
            ubo_data.cascade_splits = glm::vec4(shadow_far, 0.0f, 0.0f, 0.0f);
        }

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

        // --- Build instancing draw groups + fill InstanceBuffer ---
        const auto &inst_buf = resource_manager_->get_buffer(instance_buffers_[input.frame_index]);
        auto *gpu_instances = static_cast<framework::GPUInstanceData *>(inst_buf.allocation_info.pMappedData);
        uint32_t instance_offset = 0;

        // Camera draw groups: sort visible opaque indices by (mesh_id, alpha_mode, double_sided)
        {
            sorted_opaque_indices_.assign(
                input.cull_result.visible_opaque_indices.begin(),
                input.cull_result.visible_opaque_indices.end());
            std::ranges::sort(sorted_opaque_indices_,
                              instance_group_sort(input.mesh_instances, input.materials));

            build_draw_groups(input.mesh_instances, input.materials,
                              sorted_opaque_indices_, gpu_instances, instance_offset,
                              opaque_draw_groups_, mask_draw_groups_, true);
        }

        // --- Build shadow draw groups from ALL mesh_instances (not camera-culled) ---
        // Objects outside the camera frustum may still cast shadows onto visible surfaces.
        shadow_opaque_groups_.clear();
        shadow_mask_groups_.clear();

        if (shadows_active && !input.mesh_instances.empty()) {
            // Build sorted index list of all non-Blend instances
            sorted_shadow_indices_.clear();
            for (uint32_t i = 0; i < static_cast<uint32_t>(input.mesh_instances.size()); ++i) {
                const auto &mat = input.materials[input.mesh_instances[i].material_id];
                if (mat.alpha_mode != framework::AlphaMode::Blend) {
                    sorted_shadow_indices_.push_back(i);
                }
            }

            std::ranges::sort(sorted_shadow_indices_,
                              instance_group_sort(input.mesh_instances, input.materials));

            // Shadow shaders only read model + material_index from GPUInstanceData;
            // normal matrix computation is skipped (compute_normal_matrix = false).
            build_draw_groups(input.mesh_instances, input.materials,
                              sorted_shadow_indices_, gpu_instances, instance_offset,
                              shadow_opaque_groups_, shadow_mask_groups_, false);
        }

        // Total scene draw calls
        const auto camera_groups = static_cast<uint32_t>(
            opaque_draw_groups_.size() + mask_draw_groups_.size());
        const auto shadow_groups = static_cast<uint32_t>(
            shadow_opaque_groups_.size() + shadow_mask_groups_.size());
        draw_call_count_ = camera_groups * 2 // DepthPrePass + ForwardPass
                           + shadow_groups; // ShadowPass

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
        frame_ctx.opaque_draw_groups = opaque_draw_groups_;
        frame_ctx.mask_draw_groups = mask_draw_groups_;
        frame_ctx.shadow_opaque_groups = shadow_opaque_groups_;
        frame_ctx.shadow_mask_groups = shadow_mask_groups_;
        frame_ctx.features = &input.features;
        frame_ctx.shadow_config = &input.shadow_config;
        frame_ctx.frame_index = input.frame_index;
        frame_ctx.sample_count = current_sample_count_;

        // --- CSM Shadow Pass (before depth prepass — shadow map needed by forward) ---
        if (shadows_active) {
            shadow_pass_.record(render_graph_, frame_ctx);
        }

        // --- Depth + Normal PrePass ---
        depth_prepass_.record(render_graph_, frame_ctx);

        // --- Forward pass ---
        forward_pass_.record(render_graph_, frame_ctx);

        // --- Skybox pass (conditional on feature toggle) ---
        if (input.features.skybox) {
            skybox_pass_.record(render_graph_, frame_ctx);
        }

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

    uint32_t Renderer::last_draw_call_count() const {
        return draw_call_count_;
    }

    // ---- HDR color descriptor update ----

    void Renderer::update_hdr_color_descriptor() const {
        const auto hdr_backing = render_graph_.get_managed_backing_image(managed_hdr_color_);
        descriptor_manager_->update_render_target(0, hdr_backing, default_sampler_);
    }

    void Renderer::update_shadow_map_descriptor() const {
        descriptor_manager_->update_render_target(5,
                                                  shadow_pass_.shadow_map_image(),
                                                  shadow_comparison_sampler_);
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
