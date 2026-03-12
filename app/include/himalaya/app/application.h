#pragma once

/**
 * @file application.h
 * @brief Main application class: window management, frame loop, init/destroy sequence.
 */

#include <himalaya/app/camera_controller.h>
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
     * @brief Parsed command-line arguments passed to Application::init().
     *
     * Constructed in main() after CLI11 parsing. Application consumes
     * these values without knowing how they were parsed.
     */
    struct AppConfig {
        /** @brief Path to the glTF scene file. */
        std::string scene_path;

        /** @brief Path to the HDR environment map (used by IBL in Step 6). */
        std::string env_path;
    };

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
         * @param config Parsed command-line arguments.
         */
        void init(const AppConfig &config);

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

        /** @brief Fallback directional light when the scene provides none. */
        std::vector<framework::DirectionalLight> default_lights_;

        // --- Rendering parameters (controlled via DebugUI) ---

        /** @brief IBL environment light intensity multiplier (written to GlobalUBO each frame). */
        float ibl_intensity_ = 0.03f;

        /** @brief Exposure value in EV (pow(2, ev) gives linear exposure multiplier). */
        float ev_ = 0.0f;

        /** @brief Debug render mode (0=Final, 1+=visualization). */
        int debug_render_mode_ = 0;

        // --- Default light control ---

        /** @brief Yaw angle for the default directional light (radians). */
        float light_yaw_ = glm::radians(-45.0f);

        /** @brief Pitch angle for the default directional light (radians, negative = downward). */
        float light_pitch_ = glm::radians(-55.0f);

        /** @brief Intensity of the default directional light. */
        float light_intensity_ = 1.0f;

        /** @brief When true, use the default light even if the scene provides lights. */
        bool force_default_light_ = false;

        // --- Light direction viewport drag state ---

        /** @brief Previous cursor X for left-click light drag delta. */
        double light_last_cursor_x_ = 0.0;

        /** @brief Previous cursor Y for left-click light drag delta. */
        double light_last_cursor_y_ = 0.0;

        /** @brief Whether the left mouse button is being held for light dragging. */
        bool light_dragging_ = false;

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
         * @brief Processes left-click drag to rotate the default directional light.
         * @param using_default Whether the default light is currently active.
         *
         * Only processes input when using_default is true. Does not hide the cursor
         * (unlike right-click camera rotation).
         */
        void update_light_input(bool using_default);
    };
} // namespace himalaya::app
