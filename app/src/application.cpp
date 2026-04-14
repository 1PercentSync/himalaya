/**
 * @file application.cpp
 * @brief Application implementation: init/destroy sequence, frame loop decomposition.
 */

#include <himalaya/app/application.h>

#include <himalaya/framework/cache.h>
#include <himalaya/framework/color_utils.h>
#include <himalaya/framework/culling.h>
#include <himalaya/framework/ibl.h>
#include <himalaya/framework/mesh.h>
#include <himalaya/framework/scene_data.h>
#include <himalaya/rhi/commands.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <thread>

#include <GLFW/glfw3.h>
#include <glm/glm.hpp>
#include <imgui.h>
#include <spdlog/spdlog.h>

namespace himalaya::app {
    /** @brief Initial window width in pixels. */
    constexpr int kInitialWidth = 1920;

    /** @brief Initial window height in pixels. */
    constexpr int kInitialHeight = 1080;

    /** @brief Window title shown in the title bar. */
    constexpr auto kWindowTitle = "Himalaya";

    /** @brief Default log level used when config has no override. */
    constexpr auto kDefaultLogLevel = spdlog::level::warn;

    // ---- Init / Destroy ----

    void Application::init() {
        // Start at info so load_config() diagnostics are visible,
        // then apply the persisted level (or fall back to default warn).
        spdlog::set_level(spdlog::level::info);

        config_ = load_config();

        spdlog::set_level(config_.log_level.empty()
            ? kDefaultLogLevel
            : spdlog::level::from_str(config_.log_level));

        pt_allow_tearing_ = config_.pt_allow_tearing;
        bake_config_.allow_tearing = config_.bake_allow_tearing;
        bake_config_.spp_per_frame = config_.bake_spp_per_frame;
        bake_config_.probe_min_luminance = config_.bake_probe_min_luminance;
        resolve_thread_count();

        glfwInit();
        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);

        window_ = glfwCreateWindow(kInitialWidth, kInitialHeight, kWindowTitle, nullptr, nullptr);

        context_.init(window_);
        rhi::CommandBuffer::init_debug_functions(context_.instance);
        swapchain_.init(context_, window_, user_present_mode_);
        // Sync fallback: if requested mode was unavailable, reflect actual mode
        user_present_mode_ = swapchain_.present_mode;

        // Framebuffer resize detection via GLFW callback
        glfwSetWindowUserPointer(window_, &framebuffer_resized_);
        glfwSetFramebufferSizeCallback(window_, [](GLFWwindow *w, int, int) {
            *static_cast<bool *>(glfwGetWindowUserPointer(w)) = true;
        });

        // ImGui must be initialized after the framebuffer resize callback
        // so ImGui chains our callback when install_callbacks = true.
        imgui_backend_.init(context_, swapchain_, window_);

        resource_manager_.init(&context_);
        descriptor_manager_.init(&context_, &resource_manager_);

        // --- Camera ---
        camera_.aspect = static_cast<float>(swapchain_.extent.width) / static_cast<float>(swapchain_.extent.height);
        camera_.update_all();
        camera_controller_.init(window_, &camera_);

        // --- Renderer (owns pipelines, buffers, default textures, sampler) ---
        // HDR failure is handled internally by IBL (fallback to gray cubemap).
        renderer_.init(context_,
                       swapchain_,
                       resource_manager_,
                       descriptor_manager_,
                       imgui_backend_,
                       config_.env_path);

        // Cache env content hash (used for bake cache key computation every frame)
        if (!config_.env_path.empty()) {
            env_content_hash_ = framework::content_hash(config_.env_path);
        }

        if (config_.auto_denoise_interval > 0) {
            renderer_.auto_denoise_interval() = config_.auto_denoise_interval;
        }

        // --- Scene loading (uses Renderer's default resources) ---
        // Scene failure → empty scene (0 instances), skybox still renders if HDR loaded.
        if (!config_.scene_path.empty()) {
            context_.begin_immediate();
            const bool scene_ok = scene_loader_.load(
                config_.scene_path,
                resource_manager_,
                descriptor_manager_,
                renderer_.material_system(),
                renderer_.default_textures(),
                renderer_.default_sampler(),
                context_.rt_supported);
            context_.end_immediate();

            if (!scene_ok) {
                error_message_ = "Failed to load scene: " + config_.scene_path;
            }

            // Build RT data (acceleration structures + emissive lights) if supported
            if (scene_ok && context_.rt_supported) {
                context_.begin_immediate();
                renderer_.build_scene_rt(scene_loader_.meshes(),
                                         scene_loader_.mesh_instances(),
                                         scene_loader_.material_instances(),
                                         scene_loader_.gpu_materials(),
                                         scene_loader_.cpu_vertices(),
                                         scene_loader_.cpu_indices());
                context_.end_immediate();
            }

            // Auto-start background UV generation if configured
            if (scene_ok && config_.bg_uv_auto_start) {
                auto requests = scene_loader_.prepare_uv_requests();
                uv_generator_.start(std::move(requests), config_.bg_uv_thread_count);
            }
        }

        // Compute lightmap keys for bake progress UI.
        // switch_scene() / switch_environment() call this on runtime changes,
        // but init() loads scene/HDR directly — must be called here as well.
        refresh_lightmap_keys();

        update_shadow_config_from_scene();
        auto_position_camera();
        camera_controller_.set_focus_target(&scene_loader_.scene_bounds());

        // Auto-select light source: Scene if glTF has lights,
        // HdrSun if HDR loaded, Fallback as last resort
        if (!scene_loader_.directional_lights().empty()) {
            light_source_mode_ = LightSourceMode::Scene;
        } else if (renderer_.ibl().equirect_width() > 0) {
            light_source_mode_ = LightSourceMode::HdrSun;
        } else {
            light_source_mode_ = LightSourceMode::Fallback;
        }

        // Restore persisted HDR sun coordinates for the current environment
        if (const auto it = config_.hdr_sun_coords.find(config_.env_path);
            it != config_.hdr_sun_coords.end()) {
            hdr_sun_x_ = it->second.first;
            hdr_sun_y_ = it->second.second;
        }
    }

    void Application::auto_position_camera() {
        const auto &bounds = scene_loader_.scene_bounds();
        const float diagonal = glm::length(bounds.max - bounds.min);

        constexpr float kEpsilon = 1e-4f;
        if (diagonal < kEpsilon) return; // degenerate — keep default position

        camera_.yaw = 0.0f;
        camera_.pitch = glm::radians(-45.0f);
        camera_.position = camera_.compute_focus_position(bounds);
        camera_.update_all();
    }

    void Application::update_shadow_config_from_scene() {
        const auto &bounds = scene_loader_.scene_bounds();
        const float diagonal = glm::length(bounds.max - bounds.min);
        constexpr float kEpsilon = 1e-4f;
        if (diagonal > kEpsilon) {
            shadow_config_.max_distance = diagonal * 1.5f;
        }
        // else: keep the initialized 100m fallback
    }

    // ---- Runtime scene/environment switching ----

    void Application::switch_scene(const std::string &path) {
        uv_generator_.cancel();
        vkQueueWaitIdle(context_.graphics_queue);

        renderer_.abort_denoise();
        scene_loader_.destroy();

        if (!path.empty()) {
            context_.begin_immediate();
            const bool ok = scene_loader_.load(
                path, resource_manager_, descriptor_manager_,
                renderer_.material_system(), renderer_.default_textures(),
                renderer_.default_sampler(), context_.rt_supported);
            context_.end_immediate();

            if (!ok) {
                error_message_ = "Failed to load scene: " + path;
            } else {
                error_message_.clear();
            }

            // Build RT data (acceleration structures + emissive lights) if supported
            if (ok && context_.rt_supported) {
                context_.begin_immediate();
                renderer_.build_scene_rt(scene_loader_.meshes(),
                                         scene_loader_.mesh_instances(),
                                         scene_loader_.material_instances(),
                                         scene_loader_.gpu_materials(),
                                         scene_loader_.cpu_vertices(),
                                         scene_loader_.cpu_indices());
                context_.end_immediate();
            }

            // Auto-start background UV generation if configured
            if (ok && config_.bg_uv_auto_start) {
                auto requests = scene_loader_.prepare_uv_requests();
                uv_generator_.start(std::move(requests), config_.bg_uv_thread_count);
            }
        }

        update_shadow_config_from_scene();
        auto_position_camera();
        camera_controller_.set_focus_target(&scene_loader_.scene_bounds());

        config_.scene_path = path;
        bake_angles_dirty_ = true;
        refresh_lightmap_keys();
        save_config(config_);
    }

    void Application::switch_environment(const std::string &path) {
        vkQueueWaitIdle(context_.graphics_queue);

        const bool ok = renderer_.reload_environment(path);

        if (!ok && !path.empty()) {
            error_message_ = "Failed to load HDR: " + path;
        } else {
            error_message_.clear();
        }

        config_.env_path = path;
        env_content_hash_ = path.empty() ? std::string{} : framework::content_hash(path);
        bake_angles_dirty_ = true;
        refresh_lightmap_keys();

        // Restore persisted HDR sun coordinates for the new environment
        if (const auto it = config_.hdr_sun_coords.find(path);
            it != config_.hdr_sun_coords.end()) {
            hdr_sun_x_ = it->second.first;
            hdr_sun_y_ = it->second.second;
        } else {
            hdr_sun_x_ = 0;
            hdr_sun_y_ = 0;
        }

        save_config(config_);
    }

    void Application::start_bake_session() {
        // Wait for in-flight GPU work before destroying old VB/IB.
        // apply_lightmap_uvs() calls destroy_buffer() which frees Vulkan
        // handles immediately — in-flight frames may still reference them
        // via BLAS. Same pattern as switch_scene() / recreate_swapchain().
        vkQueueWaitIdle(context_.graphics_queue);

        // Ensure all xatlas UV caches are populated
        if (!uv_generator_.running()) {
            auto requests = scene_loader_.prepare_uv_requests();
            uv_generator_.start(std::move(requests), config_.bg_uv_thread_count);
        }
        uv_generator_.wait();

        // Rebuild all VB/IB with lightmap UVs (cache hits after generator wait)
        context_.begin_immediate();
        scene_loader_.apply_lightmap_uvs();
        context_.end_immediate();

        // Rebuild BLAS/TLAS with new VB/IB handles
        context_.begin_immediate();
        renderer_.build_scene_rt(scene_loader_.meshes(),
                                 scene_loader_.mesh_instances(),
                                 scene_loader_.material_instances(),
                                 scene_loader_.gpu_materials(),
                                 scene_loader_.cpu_vertices(),
                                 scene_loader_.cpu_indices());
        context_.end_immediate();

        // Start the bake
        context_.begin_immediate();
        renderer_.start_bake(bake_config_,
                             scene_loader_.mesh_instances(),
                             scene_loader_.meshes(),
                             scene_loader_.material_instances(),
                             scene_loader_.cpu_vertices(),
                             scene_loader_.cpu_indices(),
                             scene_loader_.scene_hash(),
                             env_content_hash_,
                             scene_loader_.scene_textures_hash(),
                             glm::degrees(ibl_yaw_),
                             render_mode_);
        context_.end_immediate();

        render_mode_ = framework::RenderMode::Baking;
    }

    void Application::refresh_lightmap_keys() {
        if (config_.scene_path.empty() || env_content_hash_.empty()) {
            return;
        }
        renderer_.compute_lightmap_keys(
            scene_loader_.mesh_instances(),
            scene_loader_.meshes(),
            scene_loader_.material_instances(),
            scene_loader_.cpu_vertices(),
            scene_loader_.cpu_indices(),
            scene_loader_.scene_hash(),
            env_content_hash_,
            scene_loader_.scene_textures_hash());
    }

    void Application::resolve_thread_count() {
        if (config_.bg_uv_thread_count == 0) {
            const auto hw = std::thread::hardware_concurrency();
            config_.bg_uv_thread_count = (hw > 4) ? (hw - 4) : 1;
            save_config(config_);
            spdlog::info("Resolved bg_uv_thread_count to {} (hardware_concurrency={})",
                         config_.bg_uv_thread_count, hw);
        }
    }

    void Application::destroy() {
        uv_generator_.cancel();
        vkQueueWaitIdle(context_.graphics_queue);

        imgui_backend_.destroy();
        scene_loader_.destroy();
        renderer_.destroy();
        descriptor_manager_.destroy();
        resource_manager_.destroy();
        swapchain_.destroy(context_.device);
        context_.destroy();
        glfwDestroyWindow(window_);
        glfwTerminate();
    }

    // ---- Frame loop ----

    void Application::run() {
        while (!glfwWindowShouldClose(window_)) {
            glfwPollEvents();

            // Pause rendering while minimized (framebuffer extent is 0)
            int fb_width = 0, fb_height = 0;
            glfwGetFramebufferSize(window_, &fb_width, &fb_height);
            while ((fb_width == 0 || fb_height == 0) && !glfwWindowShouldClose(window_)) {
                glfwWaitEvents();
                glfwGetFramebufferSize(window_, &fb_width, &fb_height);
            }

            if (!begin_frame()) continue;

            // Lightmap bake finalize: after fence wait (GPU done), before render().
            // lightmap_bake_finalize() manages its own immediate scopes internally
            // (multiple GPU→CPU→GPU round-trips: readback → OIDN → upload → BC6H → KTX2).
            if (renderer_.lightmap_finalize_pending()) {
                renderer_.lightmap_bake_finalize(
                    scene_loader_.meshes(),
                    scene_loader_.mesh_instances());
            }

            // Probe bake finalize: same pattern as lightmap finalize.
            // probe_bake_finalize() manages its own immediate scopes internally
            // (readback → per-face OIDN → upload → prefilter → BC6H → KTX2).
            if (renderer_.probe_finalize_pending()) {
                renderer_.probe_bake_finalize();
            }

            // Bake complete: restore pre-bake render mode and reset state.
            if (renderer_.bake_state() == framework::BakeState::Complete) {
                render_mode_ = renderer_.bake_pre_mode();
                renderer_.complete_bake();
                bake_angles_dirty_ = true;
            }

            update();
            render();
            end_frame();
        }
    }

    bool Application::begin_frame() {
        auto &frame = context_.current_frame();

        // Wait for the GPU to finish the previous use of this frame's resources
        VK_CHECK(vkWaitForFences(context_.device, 1, &frame.render_fence, VK_TRUE, UINT64_MAX));

        // Safe to flush deferred deletions now
        frame.deletion_queue.flush();

        // Acquire next swapchain image
        const VkResult acquire_result = vkAcquireNextImageKHR(
            context_.device, swapchain_.swapchain, UINT64_MAX,
            frame.image_available_semaphore, VK_NULL_HANDLE, &image_index_);

        if (acquire_result == VK_ERROR_OUT_OF_DATE_KHR) {
            recreate_swapchain();
            return false;
        }
        if (acquire_result != VK_SUCCESS && acquire_result != VK_SUBOPTIMAL_KHR) {
            std::abort();
        }

        // Reset fence only after a successful acquire guarantees we will submit work
        VK_CHECK(vkResetFences(context_.device, 1, &frame.render_fence));

        // Start ImGui frame
        imgui_backend_.begin_frame();

        return true;
    }

    void Application::update() {
        const float delta_time = ImGui::GetIO().DeltaTime;

        // Update camera
        camera_.aspect = static_cast<float>(swapchain_.extent.width) / static_cast<float>(swapchain_.extent.height);
        camera_controller_.update(delta_time);

        // Build fallback light from yaw/pitch each frame
        fallback_light_ = {
            .direction = {
                std::sin(fallback_light_yaw_) * std::cos(fallback_light_pitch_),
                std::sin(fallback_light_pitch_),
                -std::cos(fallback_light_yaw_) * std::cos(fallback_light_pitch_),
            },
            .color = framework::color_temperature_to_rgb(fallback_light_color_temp_),
            .intensity = fallback_light_intensity_,
            .cast_shadows = fallback_light_cast_shadows_,
        };

        // Build HDR Sun light from equirect pixel coords + IBL rotation each frame.
        // Inverse of equirect_to_cubemap.comp sample_equirect():
        //   phi   = (x/w - 0.5) * 2π   (longitude, from +X axis)
        //   theta = (0.5 - y/h) * π     (latitude, +Y = up)
        //   dir   = (cos(theta)*cos(phi), sin(theta), cos(theta)*sin(phi))
        {
            const auto &ibl = renderer_.ibl();
            const auto eq_w = static_cast<float>(ibl.equirect_width());
            const auto eq_h = static_cast<float>(ibl.equirect_height());
            glm::vec3 sun_dir{0.0f, 1.0f, 0.0f}; // default: straight up
            if (eq_w > 0.0f && eq_h > 0.0f) {
                constexpr float kPi = 3.14159265358979323846f;
                constexpr float kTwoPi = 2.0f * kPi;
                const float phi = (static_cast<float>(hdr_sun_x_) / eq_w - 0.5f) * kTwoPi;
                const float theta = (0.5f - static_cast<float>(hdr_sun_y_) / eq_h) * kPi;
                sun_dir = {
                    std::cos(theta) * std::cos(phi),
                    std::sin(theta),
                    std::cos(theta) * std::sin(phi),
                };
            }
            // Inverse of shader rotate_y: shader rotates world→HDR by +yaw,
            // we need HDR→world which is -yaw (negate sin component)
            const float s = std::sin(ibl_yaw_);
            const float c = std::cos(ibl_yaw_);
            const glm::vec3 rotated_sun{
                c * sun_dir.x - s * sun_dir.z,
                sun_dir.y,
                s * sun_dir.x + c * sun_dir.z,
            };
            hdr_sun_light_ = {
                .direction = -rotated_sun, // light travels toward scene
                .color = framework::color_temperature_to_rgb(hdr_sun_color_temp_),
                .intensity = hdr_sun_intensity_,
                .cast_shadows = hdr_sun_cast_shadows_,
            };
        }

        // Determine active lights based on light source mode
        const auto scene_lights = scene_loader_.directional_lights();
        std::span<const framework::DirectionalLight> lights;
        switch (light_source_mode_) {
            case LightSourceMode::Scene:
                lights = scene_lights;
                break;
            case LightSourceMode::Fallback:
                lights = {&fallback_light_, 1};
                break;
            case LightSourceMode::HdrSun:
                lights = {&hdr_sun_light_, 1};
                break;
            case LightSourceMode::None:
                break;
        }

        // Left-click drag: IBL rotation (no modifier) or fallback light direction (Alt)
        update_drag_input();

        // Fill SceneRenderData for culling and render
        scene_render_data_.mesh_instances = scene_loader_.mesh_instances();
        scene_render_data_.directional_lights = lights;
        scene_render_data_.camera = camera_;

        // Frustum culling + material bucketing
        perform_camera_culling();

        // Compute scene statistics for debug UI
        const auto meshes = scene_loader_.meshes();
        const auto &instances = scene_render_data_.mesh_instances;

        uint32_t rendered_triangles = 0;
        for (const auto idx: cull_result_.visible_opaque_indices)
            rendered_triangles += meshes[instances[idx].mesh_id].index_count / 3;
        for (const auto idx: cull_result_.visible_transparent_indices)
            rendered_triangles += meshes[instances[idx].mesh_id].index_count / 3;

        uint32_t total_vertices = 0;
        for (const auto &mesh: meshes)
            total_vertices += mesh.vertex_count;

        const auto visible_opaque = static_cast<uint32_t>(cull_result_.visible_opaque_indices.size());
        const auto visible_transparent = static_cast<uint32_t>(cull_result_.visible_transparent_indices.size());
        const auto total_instances = static_cast<uint32_t>(instances.size());

        // Compute light direction yaw/pitch for display
        float display_light_yaw_deg = 0.0f;
        float display_light_pitch_deg = 0.0f;
        if (light_source_mode_ == LightSourceMode::Fallback) {
            display_light_yaw_deg = glm::degrees(fallback_light_yaw_);
            display_light_pitch_deg = glm::degrees(fallback_light_pitch_);
        } else if (!lights.empty()) {
            const auto &dir = lights[0].direction;
            display_light_pitch_deg = glm::degrees(std::asin(dir.y));
            display_light_yaw_deg = glm::degrees(std::atan2(dir.x, -dir.z));
        }

        // Compute bake cache key (probe_set_key: scene + hdr + scene_textures)
        std::string bake_cache_key;
        if (!config_.scene_path.empty() && !env_content_hash_.empty()) {
            const std::string key_input = scene_loader_.scene_hash()
                + env_content_hash_
                + scene_loader_.scene_textures_hash();
            bake_cache_key = framework::content_hash(key_input.data(), key_input.size());
        }

        // Debug UI
        DebugUIContext ui_ctx{
            .delta_time = delta_time,
            .context = context_,
            .swapchain = swapchain_,
            .user_present_mode = user_present_mode_,
            .camera = camera_,
            .light_source_mode = light_source_mode_,
            .scene_has_lights = !scene_lights.empty(),
            .active_light_count = static_cast<uint32_t>(lights.size()),
            .light_yaw_deg = display_light_yaw_deg,
            .light_pitch_deg = display_light_pitch_deg,
            .ibl_rotation_deg = glm::degrees(ibl_yaw_),
            .fallback_intensity = fallback_light_intensity_,
            .fallback_cast_shadows = fallback_light_cast_shadows_,
            .fallback_color_temp = fallback_light_color_temp_,
            .hdr_sun_x = hdr_sun_x_,
            .hdr_sun_y = hdr_sun_y_,
            .hdr_sun_intensity = hdr_sun_intensity_,
            .hdr_sun_color_temp = hdr_sun_color_temp_,
            .hdr_sun_cast_shadows = hdr_sun_cast_shadows_,
            .equirect_width = renderer_.ibl().equirect_width(),
            .equirect_height = renderer_.ibl().equirect_height(),
            .render_mode = render_mode_,
            .rt_supported = context_.rt_supported,
            .pt_sample_count = renderer_.pt_sample_count(),
            .pt_config = pt_config_,
            .pt_allow_tearing = pt_allow_tearing_,
            .pt_elapsed_time = renderer_.pt_elapsed_time(),
            .denoise_enabled = renderer_.denoise_enabled(),
            .show_denoised = renderer_.show_denoised(),
            .auto_denoise = renderer_.auto_denoise(),
            .auto_denoise_interval = renderer_.auto_denoise_interval(),
            .denoise_state = renderer_.denoise_state(),
            .last_denoise_trigger_sample_count = renderer_.last_denoise_trigger_sample_count(),
            .last_denoise_duration = renderer_.last_denoise_duration(),
            .ibl_intensity = ibl_intensity_,
            .ev = ev_,
            .debug_render_mode = debug_render_mode_,
            .features = features_,
            .shadow_config = shadow_config_,
            .ao_config = ao_config_,
            .contact_shadow_config = contact_shadow_config_,
            .current_sample_count = renderer_.current_sample_count(),
            .shadow_resolution = renderer_.shadow_resolution(),
            .supported_sample_counts = context_.msaa_sample_counts,
            .scene_path = config_.scene_path,
            .env_path = config_.env_path,
            .error_message = error_message_,
            .bg_uv_thread_count = config_.bg_uv_thread_count,
            .bg_uv_auto_start = config_.bg_uv_auto_start,
            .bg_uv_running = uv_generator_.running(),
            .bg_uv_completed = uv_generator_.completed(),
            .bg_uv_total = uv_generator_.total(),
            .max_thread_count = std::thread::hardware_concurrency(),
            .bake_config = bake_config_,
            .bake_progress = renderer_.bake_progress(),
            .has_scene = !config_.scene_path.empty(),
            .has_hdr = !config_.env_path.empty(),
            .scene_aabb_longest_edge = [&] {
                const auto &b = scene_loader_.scene_bounds();
                const auto ext = b.max - b.min;
                return std::max({ext.x, ext.y, ext.z, 0.0f});
            }(),
            .bake_cache_key = bake_cache_key,
            .bake_lightmap_keys = renderer_.bake_lightmap_keys(),
            .bake_angles_dirty = bake_angles_dirty_,
            .scene_stats = {
                .total_instances = total_instances,
                .total_meshes = static_cast<uint32_t>(meshes.size()),
                .total_materials = static_cast<uint32_t>(scene_loader_.material_instances().size()),
                .total_textures = scene_loader_.texture_count(),
                .total_vertices = total_vertices,
                .visible_opaque = visible_opaque,
                .visible_transparent = visible_transparent,
                .culled = total_instances - visible_opaque - visible_transparent,
                .draw_calls = renderer_.last_draw_call_count(),
                .rendered_triangles = rendered_triangles,
            },
        };
        // ReSharper disable once CppUseStructuredBinding
        const auto actions = debug_ui_.draw(ui_ctx);

        if (actions.error_dismissed) {
            error_message_.clear();
        }

        if (actions.reload_shaders) {
            renderer_.reload_shaders();
        }

        if (actions.msaa_changed) {
            renderer_.handle_msaa_change(actions.new_sample_count);
        }

        if (actions.shadow_resolution_changed) {
            renderer_.handle_shadow_resolution_changed(actions.new_shadow_resolution);
        }

        if (actions.scene_load_requested) {
            switch_scene(actions.new_scene_path);

            // Refresh scene data after switch — the old spans and cull indices
            // are dangling because switch_scene() destroyed the previous scene.
            const auto new_scene_lights = scene_loader_.directional_lights();

            // Auto-select light source mode based on new scene
            if (!new_scene_lights.empty()) {
                light_source_mode_ = LightSourceMode::Scene;
            } else if (renderer_.ibl().equirect_width() > 0) {
                light_source_mode_ = LightSourceMode::HdrSun;
            } else {
                light_source_mode_ = LightSourceMode::Fallback;
            }

            scene_render_data_.mesh_instances = scene_loader_.mesh_instances();
            switch (light_source_mode_) {
                case LightSourceMode::Scene:
                    scene_render_data_.directional_lights = new_scene_lights;
                    break;
                case LightSourceMode::Fallback:
                    scene_render_data_.directional_lights = {&fallback_light_, 1};
                    break;
                case LightSourceMode::HdrSun:
                    scene_render_data_.directional_lights = {&hdr_sun_light_, 1};
                    break;
                case LightSourceMode::None:
                    scene_render_data_.directional_lights = {};
                    break;
            }
            perform_camera_culling();
        }

        if (actions.env_load_requested) {
            switch_environment(actions.new_env_path);
        }

        if (actions.hdr_sun_coords_changed && !config_.env_path.empty()) {
            config_.hdr_sun_coords[config_.env_path] = {hdr_sun_x_, hdr_sun_y_};
            save_config(config_);
        }

        if (actions.log_level_changed) {
            const auto level = static_cast<spdlog::level::level_enum>(actions.new_log_level);
            const auto sv = spdlog::level::to_string_view(level);
            config_.log_level = std::string(sv.data(), sv.size());
            save_config(config_);
        }

        if (actions.denoise_interval_changed) {
            renderer_.auto_denoise_interval() = actions.new_denoise_interval;
            config_.auto_denoise_interval = actions.new_denoise_interval;
            save_config(config_);
        }

        if (actions.pt_reset_requested) {
            renderer_.request_pt_reset();
        }

        if (actions.pt_denoise_requested) {
            renderer_.request_manual_denoise();
        }

        // ---- Background UV generation actions ----
        if (actions.bg_uv_start_requested) {
            auto requests = scene_loader_.prepare_uv_requests();
            uv_generator_.start(std::move(requests), config_.bg_uv_thread_count);
        }
        if (actions.bg_uv_stop_requested) {
            uv_generator_.cancel();
        }
        if (actions.bg_uv_config_changed) {
            save_config(config_);
        }

        // ---- Bake actions ----
        if (actions.bake_start_requested) {
            start_bake_session();
            bake_angles_dirty_ = true;
        }
        if (actions.bake_cancel_requested) {
            vkQueueWaitIdle(context_.graphics_queue);
            renderer_.cancel_bake();
            render_mode_ = renderer_.bake_pre_mode();
        }
        if (actions.clear_bake_cache_requested) {
            framework::clear_cache("bake");
            bake_angles_dirty_ = true;
        }
        if (actions.clear_uv_cache_requested) {
            framework::clear_cache("lightmap_uv_debug");
            framework::clear_cache("lightmap_uv_release");
        }
        if (actions.clear_all_cache_requested) {
            framework::clear_all_cache();
            bake_angles_dirty_ = true;
        }

        // ---- Effective present mode (user preference + PT/Bake tearing override) ----
        // Deferred to end_frame() after present — mid-frame recreate would
        // invalidate the acquired image and renderer's swapchain references.
        rhi::PresentMode effective = user_present_mode_;
        if ((render_mode_ == framework::RenderMode::PathTracing && pt_allow_tearing_) ||
            (render_mode_ == framework::RenderMode::Baking && bake_config_.allow_tearing)) {
            effective = rhi::PresentMode::Immediate;
        }
        if (effective != swapchain_.present_mode) {
            swapchain_.present_mode = effective;
            present_mode_changed_ = true;
        }

        // ---- Persist allow_tearing on change ----
        if (config_.pt_allow_tearing != pt_allow_tearing_) {
            config_.pt_allow_tearing = pt_allow_tearing_;
            save_config(config_);
        }
        if (config_.bake_allow_tearing != bake_config_.allow_tearing) {
            config_.bake_allow_tearing = bake_config_.allow_tearing;
            save_config(config_);
        }
        if (config_.bake_spp_per_frame != bake_config_.spp_per_frame) {
            config_.bake_spp_per_frame = bake_config_.spp_per_frame;
            save_config(config_);
        }
        if (config_.bake_probe_min_luminance != bake_config_.probe_min_luminance) {
            config_.bake_probe_min_luminance = bake_config_.probe_min_luminance;
            save_config(config_);
        }
    }

    void Application::render() {
        const auto &frame = context_.current_frame();
        rhi::CommandBuffer cmd(frame.command_buffer);
        cmd.begin();

        // Baking mode overrides: normalized IBL intensity, no directional lights
        const bool is_baking = render_mode_ == framework::RenderMode::Baking;

        const RenderInput input{
            .image_index = image_index_,
            .frame_index = context_.frame_index,
            .render_mode = render_mode_,
            .camera = camera_,
            .lights = is_baking ? std::span<const framework::DirectionalLight>{}
                                : scene_render_data_.directional_lights,
            .cull_result = cull_result_,
            .meshes = scene_loader_.meshes(),
            .materials = scene_loader_.material_instances(),
            .mesh_instances = scene_render_data_.mesh_instances,
            .ibl_intensity = is_baking ? 1.0f : ibl_intensity_,
            .exposure = std::pow(2.0f, ev_),
            .ibl_rotation_sin = std::sin(ibl_yaw_),
            .ibl_rotation_cos = std::cos(ibl_yaw_),
            .debug_render_mode = debug_render_mode_,
            .features = features_,
            .shadow_config = shadow_config_,
            .ao_config = ao_config_,
            .contact_shadow_config = contact_shadow_config_,
            .pt_config = pt_config_,
            .bake_config = bake_config_,
            .scene_bounds = scene_loader_.scene_bounds(),
        };

        renderer_.render(cmd, input);

        cmd.end();

        // Submit
        VkCommandBufferSubmitInfo cmd_submit_info{};
        cmd_submit_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
        cmd_submit_info.commandBuffer = frame.command_buffer;

        VkSemaphoreSubmitInfo wait_info{};
        wait_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
        wait_info.semaphore = frame.image_available_semaphore;
        wait_info.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;

        // Signal semaphores: render-finished + optional denoise timeline
        VkSemaphoreSubmitInfo signal_info{};
        signal_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
        signal_info.semaphore = swapchain_.render_finished_semaphores[image_index_];
        signal_info.stageMask = VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT;

        std::array<VkSemaphoreSubmitInfo, 2> signal_infos = {signal_info, {}};
        uint32_t signal_count = 1;

        const auto denoise_signal = renderer_.pending_denoise_signal();
        if (denoise_signal.semaphore != VK_NULL_HANDLE) {
            signal_infos[1].sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
            signal_infos[1].semaphore = denoise_signal.semaphore;
            signal_infos[1].value = denoise_signal.value;
            signal_infos[1].stageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
            signal_count = 2;
        }

        VkSubmitInfo2 submit_info{};
        submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
        submit_info.waitSemaphoreInfoCount = 1;
        submit_info.pWaitSemaphoreInfos = &wait_info;
        submit_info.commandBufferInfoCount = 1;
        submit_info.pCommandBufferInfos = &cmd_submit_info;
        submit_info.signalSemaphoreInfoCount = signal_count;
        submit_info.pSignalSemaphoreInfos = signal_infos.data();

        VK_CHECK(vkQueueSubmit2(context_.graphics_queue, 1, &submit_info, frame.render_fence));
    }

    void Application::end_frame() {
        VkPresentInfoKHR present_info{};
        present_info.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        present_info.waitSemaphoreCount = 1;
        present_info.pWaitSemaphores = &swapchain_.render_finished_semaphores[image_index_];
        present_info.swapchainCount = 1;
        present_info.pSwapchains = &swapchain_.swapchain;
        present_info.pImageIndices = &image_index_;

        if (const VkResult present_result = vkQueuePresentKHR(context_.graphics_queue, &present_info);
            present_result == VK_ERROR_OUT_OF_DATE_KHR ||
            present_result == VK_SUBOPTIMAL_KHR ||
            framebuffer_resized_) {
            framebuffer_resized_ = false;
            recreate_swapchain();
        } else if (present_result != VK_SUCCESS) {
            std::abort();
        }

        // Present mode change — deferred from update() to after present.
        // Must go through recreate_swapchain() for renderer swapchain hooks.
        if (present_mode_changed_) {
            present_mode_changed_ = false;
            recreate_swapchain();

            // Post-recreate: fallback may have changed swapchain_.present_mode
            if (render_mode_ == framework::RenderMode::PathTracing && pt_allow_tearing_) {
                // PT tearing override: if IMMEDIATE fell back, revert checkbox
                if (swapchain_.present_mode != rhi::PresentMode::Immediate) {
                    pt_allow_tearing_ = false;
                }
            } else if (render_mode_ == framework::RenderMode::Baking && bake_config_.allow_tearing) {
                // Bake tearing override: if IMMEDIATE fell back, revert flag
                if (swapchain_.present_mode != rhi::PresentMode::Immediate) {
                    bake_config_.allow_tearing = false;
                }
            } else {
                // User combo change: sync fallback result back to combo display
                user_present_mode_ = swapchain_.present_mode;
            }
        }

        context_.advance_frame();
    }

    // ---- Resize handling ----

    // Waits for GPU idle, invalidates renderer state, recreates swapchain
    // (resize, present mode change, etc.), then rebuilds renderer resources.
    void Application::recreate_swapchain() {
        vkQueueWaitIdle(context_.graphics_queue);
        renderer_.on_swapchain_invalidated();
        swapchain_.recreate(context_, window_);
        renderer_.on_swapchain_recreated();
    }

    // ---- Left-click drag input (IBL rotation / fallback light direction) ----

    void Application::update_drag_input() {
        const ImGuiIO &io = ImGui::GetIO();
        const bool left_pressed = !io.WantCaptureMouse &&
                                  glfwGetMouseButton(window_, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;
        const bool alt_held = glfwGetKey(window_, GLFW_KEY_LEFT_ALT) == GLFW_PRESS ||
                              glfwGetKey(window_, GLFW_KEY_RIGHT_ALT) == GLFW_PRESS;

        double cursor_x, cursor_y;
        glfwGetCursorPos(window_, &cursor_x, &cursor_y);

        if (left_pressed) {
            if (!drag_active_) {
                drag_active_ = true;
                drag_last_x_ = cursor_x;
                drag_last_y_ = cursor_y;
            }

            const auto dx = static_cast<float>(cursor_x - drag_last_x_);
            const auto dy = static_cast<float>(cursor_y - drag_last_y_);
            drag_last_x_ = cursor_x;
            drag_last_y_ = cursor_y;

            constexpr float kSensitivity = 0.003f;

            if (alt_held && light_source_mode_ == LightSourceMode::Fallback) {
                // Alt + left drag: rotate fallback light direction
                fallback_light_yaw_ += dx * kSensitivity;
                fallback_light_pitch_ -= dy * kSensitivity;
                constexpr float kMaxPitch = glm::radians(89.0f);
                fallback_light_pitch_ = std::clamp(fallback_light_pitch_, -kMaxPitch, kMaxPitch);
            } else if (!alt_held) {
                // Left drag without Alt: IBL rotation (horizontal only)
                ibl_yaw_ += dx * kSensitivity;
            }
        } else {
            drag_active_ = false;
        }
    }
    // ---- Camera frustum culling + material bucketing ----

    void Application::perform_camera_culling() {
        const auto frustum = framework::extract_frustum(camera_.view_projection);
        framework::cull_against_frustum(scene_render_data_.mesh_instances,
                                        frustum, visible_indices_);

        // Bucket visible instances by alpha mode
        cull_result_.visible_opaque_indices.clear();
        cull_result_.visible_transparent_indices.clear();

        const auto materials = scene_loader_.material_instances();
        const auto &instances = scene_render_data_.mesh_instances;

        for (const auto idx : visible_indices_) {
            if (materials[instances[idx].material_id].alpha_mode == framework::AlphaMode::Blend) {
                cull_result_.visible_transparent_indices.push_back(idx);
            } else {
                cull_result_.visible_opaque_indices.push_back(idx);
            }
        }

        // Sort transparent instances back-to-front by AABB center distance
        if (!cull_result_.visible_transparent_indices.empty()) {
            const auto cam_pos = camera_.position;
            std::ranges::sort(cull_result_.visible_transparent_indices,
                              [&](const uint32_t a, const uint32_t b) {
                                  const auto center_a = (instances[a].world_bounds.min +
                                                         instances[a].world_bounds.max) * 0.5f;
                                  const auto center_b = (instances[b].world_bounds.min +
                                                         instances[b].world_bounds.max) * 0.5f;
                                  const float dist_a = glm::dot(center_a - cam_pos, center_a - cam_pos);
                                  const float dist_b = glm::dot(center_b - cam_pos, center_b - cam_pos);
                                  return dist_a > dist_b; // far first
                              });
        }
    }
} // namespace himalaya::app
