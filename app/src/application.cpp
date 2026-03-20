/**
 * @file application.cpp
 * @brief Application implementation: init/destroy sequence, frame loop decomposition.
 */

#include <himalaya/app/application.h>

#include <himalaya/framework/culling.h>
#include <himalaya/framework/mesh.h>
#include <himalaya/framework/scene_data.h>
#include <himalaya/rhi/commands.h>

#include <algorithm>
#include <cmath>

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

    /** @brief Default log level. Change to debug/info for more verbose Vulkan diagnostics. */
    constexpr auto kLogLevel = spdlog::level::warn;

    // ---- Init / Destroy ----

    void Application::init() {
        spdlog::set_level(kLogLevel);

        config_ = load_config();

        glfwInit();
        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);

        window_ = glfwCreateWindow(kInitialWidth, kInitialHeight, kWindowTitle, nullptr, nullptr);

        context_.init(window_);
        rhi::CommandBuffer::init_debug_functions(context_.instance);
        swapchain_.init(context_, window_);

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
                renderer_.default_sampler());
            context_.end_immediate();

            if (!scene_ok) {
                error_message_ = "Failed to load scene: " + config_.scene_path;
            }
        }

        update_shadow_config_from_scene();
        auto_position_camera();
        camera_controller_.set_focus_target(&scene_loader_.scene_bounds());

        // Auto-select light source: Scene if glTF has lights, Fallback otherwise
        light_source_mode_ = scene_loader_.directional_lights().empty()
                                 ? LightSourceMode::Fallback
                                 : LightSourceMode::Scene;
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
        vkQueueWaitIdle(context_.graphics_queue);

        scene_loader_.destroy();

        if (!path.empty()) {
            context_.begin_immediate();
            const bool ok = scene_loader_.load(
                path, resource_manager_, descriptor_manager_,
                renderer_.material_system(), renderer_.default_textures(),
                renderer_.default_sampler());
            context_.end_immediate();

            if (!ok) {
                error_message_ = "Failed to load scene: " + path;
            } else {
                error_message_.clear();
            }
        }

        update_shadow_config_from_scene();
        auto_position_camera();
        camera_controller_.set_focus_target(&scene_loader_.scene_bounds());

        config_.scene_path = path;
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
        save_config(config_);
    }

    void Application::destroy() {
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
            handle_resize();
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
            .color = glm::vec3(1.0f),
            .intensity = fallback_light_intensity_,
            .cast_shadows = fallback_light_cast_shadows_,
        };

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

        // Debug UI
        DebugUIContext ui_ctx{
            .delta_time = delta_time,
            .context = context_,
            .swapchain = swapchain_,
            .camera = camera_,
            .light_source_mode = light_source_mode_,
            .scene_has_lights = !scene_lights.empty(),
            .active_light_count = static_cast<uint32_t>(lights.size()),
            .light_yaw_deg = display_light_yaw_deg,
            .light_pitch_deg = display_light_pitch_deg,
            .ibl_rotation_deg = glm::degrees(ibl_yaw_),
            .fallback_intensity = fallback_light_intensity_,
            .fallback_cast_shadows = fallback_light_cast_shadows_,
            .ibl_intensity = ibl_intensity_,
            .ev = ev_,
            .debug_render_mode = debug_render_mode_,
            .features = features_,
            .shadow_config = shadow_config_,
            .current_sample_count = renderer_.current_sample_count(),
            .supported_sample_counts = context_.msaa_sample_counts,
            .scene_path = config_.scene_path,
            .env_path = config_.env_path,
            .error_message = error_message_,
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

        if (actions.vsync_toggled) {
            vsync_changed_ = true;
        }

        if (actions.reload_shaders) {
            renderer_.reload_shaders();
        }

        if (actions.msaa_changed) {
            renderer_.handle_msaa_change(actions.new_sample_count);
        }

        if (actions.scene_load_requested) {
            switch_scene(actions.new_scene_path);

            // Refresh scene data after switch — the old spans and cull indices
            // are dangling because switch_scene() destroyed the previous scene.
            const auto new_scene_lights = scene_loader_.directional_lights();

            // Auto-select light source mode based on new scene
            if (!new_scene_lights.empty()) {
                light_source_mode_ = LightSourceMode::Scene;
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
                case LightSourceMode::None:
                    scene_render_data_.directional_lights = {};
                    break;
            }
            perform_camera_culling();
        }

        if (actions.env_load_requested) {
            switch_environment(actions.new_env_path);
        }
    }

    void Application::render() {
        const auto &frame = context_.current_frame();
        rhi::CommandBuffer cmd(frame.command_buffer);
        cmd.begin();

        const RenderInput input{
            .image_index = image_index_,
            .frame_index = context_.frame_index,
            .camera = camera_,
            .lights = scene_render_data_.directional_lights,
            .cull_result = cull_result_,
            .meshes = scene_loader_.meshes(),
            .materials = scene_loader_.material_instances(),
            .mesh_instances = scene_render_data_.mesh_instances,
            .ibl_intensity = ibl_intensity_,
            .exposure = std::pow(2.0f, ev_),
            .ibl_rotation_sin = std::sin(ibl_yaw_),
            .ibl_rotation_cos = std::cos(ibl_yaw_),
            .debug_render_mode = debug_render_mode_,
            .features = features_,
            .shadow_config = shadow_config_,
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

        VkSemaphoreSubmitInfo signal_info{};
        signal_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
        signal_info.semaphore = swapchain_.render_finished_semaphores[image_index_];
        signal_info.stageMask = VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT;

        VkSubmitInfo2 submit_info{};
        submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
        submit_info.waitSemaphoreInfoCount = 1;
        submit_info.pWaitSemaphoreInfos = &wait_info;
        submit_info.commandBufferInfoCount = 1;
        submit_info.pCommandBufferInfos = &cmd_submit_info;
        submit_info.signalSemaphoreInfoCount = 1;
        submit_info.pSignalSemaphoreInfos = &signal_info;

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
            framebuffer_resized_ ||
            vsync_changed_) {
            framebuffer_resized_ = false;
            vsync_changed_ = false;
            handle_resize();
        } else if (present_result != VK_SUCCESS) {
            std::abort();
        }

        context_.advance_frame();
    }

    // ---- Resize handling ----

    // vkQueueWaitIdle guarantees no GPU references, so we destroy immediately
    // (not deferred). All resolution-dependent resources are rebuilt after
    // the swapchain is recreated.
    void Application::handle_resize() {
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
