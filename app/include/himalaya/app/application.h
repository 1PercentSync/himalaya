#pragma once

/**
 * @file application.h
 * @brief Main application class: window management, frame loop, init/destroy sequence.
 */

#include <himalaya/app/camera_controller.h>
#include <himalaya/app/config.h>
#include <himalaya/app/debug_ui.h>
#include <himalaya/app/renderer.h>
#include <himalaya/app/scene_loader.h>
#include <himalaya/framework/camera.h>
#include <himalaya/framework/gaussian_splat_data.h>
#include <himalaya/framework/scene_data.h>
#include <himalaya/framework/imgui_backend.h>
#include <himalaya/rhi/context.h>
#include <himalaya/rhi/descriptors.h>
#include <himalaya/rhi/resources.h>
#include <himalaya/rhi/swapchain.h>

#include <optional>

struct GLFWwindow;

namespace himalaya::app {
    /**
     * @brief Top-level application managing the window, subsystems, and frame loop.
     *
     * Owns all RHI and framework subsystems. The frame loop is decomposed into
     * begin_frame(), update(), render(), and end_frame() private methods.
     * Lifetime is managed via init() and destroy().
     */
    class Application {
    public:
        /**
         * @brief Initializes GLFW, all subsystems, and loads the scene.
         *
         * Reads AppConfig from disk (config.json) to determine scene and
         * environment paths. Missing or invalid config uses empty defaults.
         */
        void init();

        /**
         * @brief Runs the main frame loop until the window is closed.
         *
         * Each iteration: poll events, handle minimize pause, then
         * begin_frame → update → render → end_frame.
         */
        void run();

        /** @brief Destroys all resources and subsystems in reverse init order. */
        void destroy();

    private:
        // --- Window ---

        /** @brief GLFW window handle. */
        GLFWwindow *window_ = nullptr;

        /** @brief Set by the GLFW framebuffer size callback when a resize occurs. */
        bool framebuffer_resized_ = false;

        // --- RHI infrastructure ---

        /** @brief Vulkan context: instance, device, queues, allocator. */
        rhi::Context context_{};

        /** @brief Swapchain: presentation surface, images, and image views. */
        rhi::Swapchain swapchain_{};

        /** @brief GPU resource pool: buffers, images, and samplers. */
        rhi::ResourceManager resource_manager_{};

        /** @brief Descriptor set layouts, pools, and bindless texture management. */
        rhi::DescriptorManager descriptor_manager_{};

        // --- Framework ---

        /** @brief ImGui integration backend. */
        framework::ImGuiBackend imgui_backend_{};

        // --- App modules ---

        /** @brief Camera state (position, orientation, matrices). */
        framework::Camera camera_{};

        /** @brief Free-roaming camera controller. */
        CameraController camera_controller_{};

        /** @brief Debug UI panel. */
        DebugUI debug_ui_{};

        /** @brief glTF scene loader and resource owner (PT entry). */
        SceneLoader scene_loader_{};

        /** @brief Loaded Gaussian Splatting scene data (GS entry). */
        std::optional<framework::GaussianSplatScene> gs_scene_;

        // --- Rendering parameters (controlled via DebugUI) ---

        /** @brief Indirect light intensity multiplier (written to GlobalUBO each frame). */
        float indirect_intensity_ = 1.0f;

        /** @brief Exposure value in EV (pow(2, ev) gives linear exposure multiplier). */
        float ev_ = 0.0f;

        /** @brief GS projection-stability near distance in world units. */
        float gs_near_plane_ = 0.25f;

        // --- IBL rotation ---

        /** @brief IBL horizontal rotation angle in radians (left-click drag controlled). */
        float ibl_yaw_ = 0.0f;

        /** @brief Application-selected render path for the current frame. */
        framework::RenderMode render_mode_ = framework::RenderMode::PathTracing;

        /** @brief Path tracing configuration parameters. */
        framework::PTConfig pt_config_{};

        // --- Left-click drag state (IBL rotation) ---

        /** @brief Previous cursor X for left-click drag delta. */
        double drag_last_x_ = 0.0;

        /** @brief Whether the left mouse button is being held for dragging. */
        bool drag_active_ = false;

        // --- Rendering ---

        /** @brief Rendering subsystem (owns pipelines, buffers, shared resources). */
        Renderer renderer_{};

        /** @brief Deferred present mode change flag — handled in end_frame() after present. */
        bool present_mode_changed_ = false;

        /** @brief User-selected present mode from combo (may differ from effective when PT tearing overrides). */
        rhi::PresentMode user_present_mode_ = rhi::PresentMode::Mailbox;

        /** @brief PT allow tearing: override to IMMEDIATE while in PT mode. */
        bool pt_allow_tearing_ = false;

        /** @brief Acquired swapchain image index for the current frame. */
        uint32_t image_index_ = 0;

        // --- Frame loop phases ---

        /**
         * @brief Waits for the previous frame's fence, flushes deferred deletions,
         *        acquires the next swapchain image, and begins ImGui frame.
         * @return true if the frame should proceed, false if acquire failed (retry next iteration).
         */
        bool begin_frame();

        /**
         * @brief Processes per-frame updates: debug panel, input, etc.
         */
        void update();

        /**
         * @brief Records and submits the command buffer for the current frame.
         */
        void render();

        /**
         * @brief Presents the rendered image and handles swapchain recreation if needed.
         */
        void end_frame();

        /**
         * @brief Handles window resize: waits for GPU idle, destroys old resolution-dependent
         *        resources, recreates swapchain, and rebuilds those resources.
         */
        void recreate_swapchain();

        /**
         * @brief Processes left-click drag input.
         *
         * Left-click drag rotates IBL environment horizontally (ibl_yaw_).
         */
        void update_drag_input();

        /**
         * @brief Positions the camera to overlook the given bounds.
         *
         * Sets yaw=0, pitch=-45 degrees, and computes position from AABB
         * using compute_focus_position(). No-op if the AABB is degenerate.
         *
         * @param bounds Scene AABB to focus on.
         */
        void auto_position_camera(const framework::AABB &bounds);

        // --- Runtime scene/environment switching ---

        /**
         * @brief Switches to a new glTF scene file.
         *
         * Waits for GPU idle, destroys current scene, loads new scene,
         * and saves config. On failure, remains with an empty scene.
         *
         * @param path Absolute path to the new .gltf / .glb file.
         */
        void switch_scene(const std::string &path);

        /**
         * @brief Switches to a new GS scene file.
         *
         * Loads .gltf/.glb via GaussianSplatLoader, .ply via PLY converter
         * then GaussianSplatLoader. On failure, remains with no GS scene.
         *
         * @param path Absolute path to .gltf, .glb, or .ply file.
         */
        void switch_gs_scene(const std::string &path);

        /**
         * @brief Switches to a new HDR environment map.
         *
         * Waits for GPU idle, destroys current IBL, loads new environment,
         * and saves config. On failure, IBL falls back to gray cubemaps.
         *
         * @param path Absolute path to the new .hdr file.
         */
        void switch_environment(const std::string &path);

        /** @brief Persistent config (updated on scene/environment switch). */
        AppConfig config_{};

        /** @brief Error message shown in DebugUI (empty = no error, auto-dismissed after timeout). */
        std::string error_message_;
    };
} // namespace himalaya::app
