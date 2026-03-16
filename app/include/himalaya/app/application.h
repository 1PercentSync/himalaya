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
#include <himalaya/framework/imgui_backend.h>
#include <himalaya/rhi/context.h>
#include <himalaya/rhi/descriptors.h>
#include <himalaya/rhi/resources.h>
#include <himalaya/rhi/swapchain.h>

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

        /** @brief glTF scene loader and resource owner. */
        SceneLoader scene_loader_{};

        /** @brief Per-frame scene render data (populated in update()). */
        framework::SceneRenderData scene_render_data_{};

        /** @brief Per-frame frustum culling result (populated in update()). */
        framework::CullResult cull_result_{};

        // --- Rendering parameters (controlled via DebugUI) ---

        /** @brief IBL environment light intensity multiplier (written to GlobalUBO each frame). */
        float ibl_intensity_ = 1.0f;

        /** @brief Exposure value in EV (pow(2, ev) gives linear exposure multiplier). */
        float ev_ = 0.0f;

        // --- IBL rotation ---

        /** @brief IBL horizontal rotation angle in radians (left-click drag controlled). */
        float ibl_yaw_ = 0.0f;

        /** @brief When true, scene directional lights are disabled (IBL only). */
        bool disable_scene_lights_ = false;

        /** @brief Debug render mode (0=Full PBR, 1-7=debug visualizations). */
        uint32_t debug_render_mode_ = 0;

        // --- IBL rotation viewport drag state ---

        /** @brief Previous cursor X for left-click IBL drag delta. */
        double drag_last_x_ = 0.0;

        /** @brief Whether the left mouse button is being held for IBL dragging. */
        bool drag_active_ = false;

        // --- Rendering ---

        /** @brief Rendering subsystem (owns pipelines, buffers, shared resources). */
        Renderer renderer_{};

        /** @brief Whether VSync was toggled this frame (triggers swapchain recreate). */
        bool vsync_changed_ = false;

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
         *
         * Called from both begin_frame() (acquire failure) and end_frame() (present failure
         * or explicit resize/vsync toggle). Uses vkQueueWaitIdle for immediate destruction
         * instead of deferred deletion, since idle guarantees no GPU references.
         */
        void handle_resize();

        /**
         * @brief Processes left-click drag to rotate the IBL environment horizontally.
         *
         * Updates ibl_yaw_ based on horizontal cursor delta. Does not hide
         * the cursor (unlike right-click camera rotation).
         */
        void update_ibl_input();
    };
} // namespace himalaya::app
