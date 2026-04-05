/**
 * @file renderer.cpp
 * @brief Renderer core: render() dispatch, fill_common_gpu_data(), accessors.
 */

#include <himalaya/app/renderer.h>

#include <himalaya/framework/scene_data.h>
#include <himalaya/framework/shadow.h>
#include <himalaya/rhi/descriptors.h>
#include <himalaya/rhi/resources.h>
#include <himalaya/rhi/swapchain.h>

#include <array>
#include <cmath>

#include <GLFW/glfw3.h>

namespace himalaya::app {
    /**
     * @brief Maximum directional lights the LightBuffer can hold.
     */
    constexpr uint32_t kMaxDirectionalLights = 1;

    // ---- GPU data fill ----

    void Renderer::fill_common_gpu_data(const RenderInput &input) const {
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

        // Find the first shadow-casting directional light
        const framework::DirectionalLight *shadow_light = nullptr;
        for (const auto &light: input.lights) {
            if (light.cast_shadows) {
                shadow_light = &light;
                break;
            }
        }
        const bool shadows_active = input.features.shadows && shadow_light;

        // --- Feature flags ---
        ubo_data.feature_flags = 0;
        if (shadows_active) {
            ubo_data.feature_flags |= 1u << 0; // FEATURE_SHADOWS
        }
        if (input.features.ao) {
            ubo_data.feature_flags |= 1u << 1; // FEATURE_AO
        }
        if (input.features.contact_shadows) {
            ubo_data.feature_flags |= 1u << 2; // FEATURE_CONTACT_SHADOWS
        }

        // --- Phase 5 matrices ---
        ubo_data.inv_projection = glm::inverse(input.camera.projection);
        ubo_data.prev_view_projection = prev_view_projection_;
        ubo_data.frame_index = frame_counter_;
        ubo_data.ao_so_mode = input.ao_config.use_gtso ? 1u : 0u;

        // --- Phase 6 matrices ---
        ubo_data.inv_view = glm::inverse(input.camera.view);

        // --- Shadow fields ---
        ubo_data.shadow_normal_offset = input.shadow_config.normal_offset;
        ubo_data.shadow_texel_size = 1.0f / static_cast<float>(shadow_pass_.resolution());
        ubo_data.shadow_max_distance = input.shadow_config.max_distance;
        ubo_data.shadow_blend_width = input.shadow_config.blend_width;
        ubo_data.shadow_distance_fade_width = input.shadow_config.distance_fade_width;
        ubo_data.shadow_pcf_radius = input.shadow_config.pcf_radius;

        if (shadows_active) {
            ubo_data.shadow_cascade_count = input.shadow_config.cascade_count;

            const auto cascades = framework::compute_shadow_cascades(
                input.camera,
                // ReSharper disable once CppDFANullDereference
                glm::normalize(shadow_light->direction),
                input.shadow_config,
                input.scene_bounds,
                ubo_data.shadow_texel_size);

            std::memcpy(ubo_data.cascade_view_proj, cascades.cascade_view_proj,
                        sizeof(ubo_data.cascade_view_proj));
            ubo_data.cascade_splits = cascades.cascade_splits;
            ubo_data.cascade_texel_world_size = cascades.cascade_texel_world_size;

            // --- PCSS per-cascade parameters ---
            ubo_data.shadow_mode = input.shadow_config.shadow_mode;
            ubo_data.pcss_flags = input.shadow_config.pcss_flags;

            constexpr uint32_t kBlockerSamples[] = {16, 16, 32};
            constexpr uint32_t kPcfSamples[] = {16, 25, 49};
            const uint32_t qi = std::min(input.shadow_config.pcss_quality, 2u);
            ubo_data.pcss_blocker_samples = kBlockerSamples[qi];
            ubo_data.pcss_pcf_samples = kPcfSamples[qi];

            const float half_tan = std::tan(input.shadow_config.light_angular_diameter * 0.5f);
            const float two_half_tan = 2.0f * half_tan;
            const auto n = static_cast<int>(input.shadow_config.cascade_count);
            for (int i = 0; i < n; ++i) {
                constexpr float kMinExtent = 1e-6f;
                const float wx = std::max(cascades.cascade_width_x[i], kMinExtent);
                const float wy = std::max(cascades.cascade_width_y[i], kMinExtent);
                ubo_data.cascade_light_size_uv[i] = two_half_tan / wx;
                ubo_data.cascade_pcss_scale[i] = cascades.cascade_depth_range[i] * two_half_tan / wx;
                ubo_data.cascade_uv_scale_y[i] = wx / wy;
            }
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
    }

    // ---- Render dispatch ----

    void Renderer::render(rhi::CommandBuffer &cmd, const RenderInput &input) {
        pending_semaphore_signal_ = {}; // Clear previous frame's signal
        fill_common_gpu_data(input);

        // Fall back to rasterization when PT is requested but no valid TLAS exists
        // (all scene primitives degenerate, or scene not loaded yet).
        const bool can_path_trace = input.render_mode == framework::RenderMode::PathTracing
                                    && scene_as_builder_.tlas_handle().as != VK_NULL_HANDLE;

        if (can_path_trace) {
            render_path_tracing(cmd, input);
        } else {
            update_hdr_color_descriptor();
            render_rasterization(cmd, input);
        }

        // Cache current VP for next frame's temporal reprojection
        prev_view_projection_ = input.camera.view_projection;
        ++frame_counter_;
    }

    // ---- Accessors ----

    const framework::IBL &Renderer::ibl() const {
        return ibl_;
    }

    uint32_t Renderer::current_sample_count() const {
        return current_sample_count_;
    }

    uint32_t Renderer::shadow_resolution() const {
        return shadow_pass_.resolution();
    }

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

    framework::Denoiser::SemaphoreSignal Renderer::pending_denoise_signal() const {
        return pending_semaphore_signal_;
    }

    void Renderer::abort_denoise() {
        denoiser_.abort();
        reset_pt_accumulation();
    }

    void Renderer::reset_pt_accumulation() {
        upload_pending_completion_ = false;
        reference_view_pass_.reset_accumulation();
        ++accumulation_generation_;
        last_denoised_sample_count_ = 0;
    }
} // namespace himalaya::app
