/**
 * @file application.cpp
 * @brief Application implementation: init/destroy sequence, frame loop decomposition.
 */

#include <himalaya/app/application.h>

#include <himalaya/app/gaussian_splat_loader.h>
#include <himalaya/framework/cache.h>
#include <himalaya/framework/ply_converter.h>
#include <himalaya/framework/scene_data.h>
#include <himalaya/rhi/commands.h>

#include <array>
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

        glfwInit();
        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);

        window_ = glfwCreateWindow(kInitialWidth, kInitialHeight, kWindowTitle, nullptr, nullptr);

        context_.init(window_);
        rhi::CommandBuffer::init_debug_functions(context_.instance);
        swapchain_.init(context_, window_, user_present_mode_);
        user_present_mode_ = swapchain_.present_mode;

        glfwSetWindowUserPointer(window_, &framebuffer_resized_);
        glfwSetFramebufferSizeCallback(window_, [](GLFWwindow *w, int, int) {
            *static_cast<bool *>(glfwGetWindowUserPointer(w)) = true;
        });

        imgui_backend_.init(context_, swapchain_, window_);

        resource_manager_.init(&context_);
        descriptor_manager_.init(&context_, &resource_manager_);

        // --- Camera ---
        camera_.aspect = static_cast<float>(swapchain_.extent.width) / static_cast<float>(swapchain_.extent.height);
        camera_.update_all();
        camera_controller_.init(window_, &camera_);

        // --- Renderer ---
        renderer_.init(context_,
                       swapchain_,
                       resource_manager_,
                       descriptor_manager_,
                       imgui_backend_,
                       config_.env_path);

        if (config_.auto_denoise_interval > 0) {
            renderer_.auto_denoise_interval() = config_.auto_denoise_interval;
        }

        // --- Scene loading ---
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
        }

        // --- GS scene loading ---
        if (!config_.gs_scene_path.empty()) {
            switch_gs_scene(config_.gs_scene_path);
        }

        // Camera: position based on current render mode
        if (!pt_mode_ && gs_scene_) {
            auto_position_camera(gs_scene_->scene_bounds);
            camera_controller_.set_focus_target(&gs_scene_->scene_bounds);
        } else {
            auto_position_camera(scene_loader_.scene_bounds());
            camera_controller_.set_focus_target(&scene_loader_.scene_bounds());
        }
    }

    void Application::auto_position_camera(const framework::AABB &bounds) {
        const float diagonal = glm::length(bounds.max - bounds.min);

        constexpr float kEpsilon = 1e-4f;
        if (diagonal < kEpsilon) { return; }

        camera_.yaw = 0.0f;
        camera_.pitch = glm::radians(-45.0f);
        camera_.position = camera_.compute_focus_position(bounds);
        camera_.update_all();
    }

    // ---- Runtime scene/environment switching ----

    void Application::switch_scene(const std::string &path) {
        vkQueueWaitIdle(context_.graphics_queue);

        renderer_.abort_denoise();
        renderer_.destroy_scene_rt();
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
        }

        if (pt_mode_) {
            auto_position_camera(scene_loader_.scene_bounds());
            camera_controller_.set_focus_target(&scene_loader_.scene_bounds());
        }

        config_.scene_path = path;
        save_config(config_);
    }

    void Application::switch_gs_scene(const std::string &path) {
        vkQueueWaitIdle(context_.graphics_queue);

        renderer_.destroy_gaussian_splat_scene();
        gs_scene_.reset();
        camera_controller_.set_focus_target(&scene_loader_.scene_bounds());

        if (!path.empty()) {
            std::filesystem::path gltf_path = path;

            // PLY auto-conversion: convert to cached glTF first
            if (gltf_path.extension() == ".ply") {
                try {
                    const auto hash = framework::content_hash(gltf_path);
                    if (hash.empty()) {
                        throw std::runtime_error("Failed to hash PLY file");
                    }
                    auto cached = framework::cache_path("gaussians", hash, ".gltf");
                    auto cached_bin = framework::cache_path("gaussians", hash, ".bin");
                    if (!std::filesystem::exists(cached) || !std::filesystem::exists(cached_bin)) {
                        framework::convert_ply_to_gltf(gltf_path, cached);
                    } else {
                        spdlog::info("PLY cache hit: {}", cached.string());
                    }
                    gltf_path = cached;
                } catch (const std::exception &e) {
                    spdlog::error("PLY conversion failed: {}: {}", path, e.what());
                    error_message_ = "PLY conversion failed: " + std::string(e.what());
                    config_.gs_scene_path = path;
                    save_config(config_);
                    return;
                }
            }

            auto result = gaussian_splat_loader::load(gltf_path);

            // Cache corruption: delete cached file and retry conversion
            if (!result && gltf_path != std::filesystem::path(path)) {
                spdlog::warn("Cached glTF load failed, retrying conversion: {}", path);
                std::error_code ec;
                std::filesystem::remove(gltf_path, ec);
                auto bin_path = gltf_path;
                bin_path.replace_extension(".bin");
                std::filesystem::remove(bin_path, ec);

                try {
                    framework::convert_ply_to_gltf(path, gltf_path);
                    result = gaussian_splat_loader::load(gltf_path);
                } catch (const std::exception &e) {
                    spdlog::error("PLY conversion failed (retry): {}: {}", path, e.what());
                    error_message_ = "PLY conversion failed: " + std::string(e.what());
                    config_.gs_scene_path = path;
                    save_config(config_);
                    return;
                }
            }

            if (!result) {
                spdlog::error("Failed to load GS scene: {}", path);
                error_message_ = "Failed to load GS scene: " + path;
            } else {
                std::string build_error;
                if (!renderer_.build_gaussian_splat_scene(*result, build_error)) {
                    spdlog::error("Failed to build GS scene: {}: {}", path, build_error);
                    renderer_.destroy_gaussian_splat_scene();
                    gs_scene_.reset();
                    error_message_ = "Failed to build GS scene: " + build_error;
                } else {
                    error_message_.clear();
                    gs_scene_ = std::move(result);
                    spdlog::info("Loaded GS scene: {} ({} primitives)",
                                 path, gs_scene_->primitives.size());

                    if (!pt_mode_) {
                        auto_position_camera(gs_scene_->scene_bounds);
                        camera_controller_.set_focus_target(&gs_scene_->scene_bounds);
                    }
                }
            }
        }

        config_.gs_scene_path = path;
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
        renderer_.destroy();
        gs_scene_.reset();
        scene_loader_.destroy();
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

            int fb_width = 0, fb_height = 0;
            glfwGetFramebufferSize(window_, &fb_width, &fb_height);
            while ((fb_width == 0 || fb_height == 0) && !glfwWindowShouldClose(window_)) {
                glfwWaitEvents();
                glfwGetFramebufferSize(window_, &fb_width, &fb_height);
            }

            if (!begin_frame()) { continue; }

            update();
            render();
            end_frame();
        }
    }

    bool Application::begin_frame() {
        auto &frame = context_.current_frame();

        VK_CHECK(vkWaitForFences(context_.device, 1, &frame.render_fence, VK_TRUE, UINT64_MAX));
        frame.deletion_queue.flush();

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

        VK_CHECK(vkResetFences(context_.device, 1, &frame.render_fence));
        imgui_backend_.begin_frame();

        return true;
    }

    void Application::update() {
        const float delta_time = ImGui::GetIO().DeltaTime;

        camera_.aspect = static_cast<float>(swapchain_.extent.width) / static_cast<float>(swapchain_.extent.height);
        camera_controller_.update(delta_time);

        // Left-click drag: IBL rotation
        update_drag_input();

        const auto meshes = scene_loader_.meshes();
        uint32_t total_vertices = 0;
        uint32_t total_triangles = 0;
        for (const auto &mesh: meshes) {
            total_vertices += mesh.vertex_count;
            total_triangles += mesh.index_count / 3;
        }

        // Debug UI
        DebugUIContext ui_ctx{
            .delta_time = delta_time,
            .context = context_,
            .swapchain = swapchain_,
            .user_present_mode = user_present_mode_,
            .camera = camera_,
            .ibl_rotation_deg = glm::degrees(ibl_yaw_),
            .pt_mode = pt_mode_,
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
            .indirect_intensity = indirect_intensity_,
            .ev = ev_,
            .scene_path = config_.scene_path,
            .gs_scene_path = config_.gs_scene_path,
            .env_path = config_.env_path,
            .error_message = error_message_,
            .scene_stats = {
                .total_instances = static_cast<uint32_t>(scene_loader_.mesh_instances().size()),
                .total_meshes = static_cast<uint32_t>(meshes.size()),
                .total_materials = static_cast<uint32_t>(scene_loader_.material_instances().size()),
                .total_textures = scene_loader_.texture_count(),
                .total_vertices = total_vertices,
                .total_triangles = total_triangles,
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

        if (actions.scene_load_requested) {
            switch_scene(actions.new_scene_path);
        }

        if (actions.gs_scene_load_requested) {
            switch_gs_scene(actions.new_gs_scene_path);
        }

        if (actions.env_load_requested) {
            switch_environment(actions.new_env_path);
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

        // ---- Effective present mode (user preference + PT tearing override) ----
        rhi::PresentMode effective = user_present_mode_;
        if (pt_allow_tearing_) {
            effective = rhi::PresentMode::Immediate;
        }
        if (effective != swapchain_.present_mode) {
            swapchain_.present_mode = effective;
            present_mode_changed_ = true;
        }

        if (config_.pt_allow_tearing != pt_allow_tearing_) {
            config_.pt_allow_tearing = pt_allow_tearing_;
            save_config(config_);
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
            .indirect_intensity = indirect_intensity_,
            .exposure = std::pow(2.0f, ev_),
            .ibl_rotation_sin = std::sin(ibl_yaw_),
            .ibl_rotation_cos = std::cos(ibl_yaw_),
            .pt_config = pt_config_,
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

        if (present_mode_changed_) {
            present_mode_changed_ = false;
            recreate_swapchain();

            if (pt_allow_tearing_ && swapchain_.present_mode != rhi::PresentMode::Immediate) {
                pt_allow_tearing_ = false;
            }
            if (!pt_allow_tearing_) {
                user_present_mode_ = swapchain_.present_mode;
            }
        }

        context_.advance_frame();
    }

    // ---- Resize handling ----

    void Application::recreate_swapchain() {
        vkQueueWaitIdle(context_.graphics_queue);
        renderer_.on_swapchain_invalidated();
        swapchain_.recreate(context_, window_);
        renderer_.on_swapchain_recreated();
    }

    // ---- Left-click drag input (IBL rotation) ----

    void Application::update_drag_input() {
        const ImGuiIO &io = ImGui::GetIO();
        const bool left_pressed = !io.WantCaptureMouse &&
                                  glfwGetMouseButton(window_, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;

        double cursor_x, cursor_y;
        glfwGetCursorPos(window_, &cursor_x, &cursor_y);

        if (left_pressed) {
            if (!drag_active_) {
                drag_active_ = true;
                drag_last_x_ = cursor_x;
            }

            const auto dx = static_cast<float>(cursor_x - drag_last_x_);
            drag_last_x_ = cursor_x;

            constexpr float kSensitivity = 0.003f;
            ibl_yaw_ += dx * kSensitivity;
        } else {
            drag_active_ = false;
        }
    }
} // namespace himalaya::app
