#pragma once

/**
 * @file application.h
 * @brief Main application class: window management, frame loop, init/destroy sequence.
 */

#include <himalaya/app/debug_ui.h>
#include <himalaya/framework/imgui_backend.h>
#include <himalaya/rhi/context.h>
#include <himalaya/rhi/pipeline.h>
#include <himalaya/rhi/resources.h>
#include <himalaya/rhi/shader.h>
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
        /** @brief Initializes GLFW, all subsystems, and temporary resources. */
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
        GLFWwindow* window_ = nullptr;

        /** @brief Set by the GLFW framebuffer size callback when a resize occurs. */
        bool framebuffer_resized_ = false;

        // --- RHI infrastructure ---

        /** @brief Vulkan context: instance, device, queues, allocator. */
        rhi::Context context_;

        /** @brief Swapchain: presentation surface, images, and image views. */
        rhi::Swapchain swapchain_;

        /** @brief GPU resource pool: buffers and images. */
        rhi::ResourceManager resource_manager_;

        // --- Framework ---

        /** @brief ImGui integration backend. */
        framework::ImGuiBackend imgui_backend_;

        // --- App modules ---

        /** @brief Debug UI panel. */
        DebugUI debug_ui_;

        // --- Phase 1 temporary resources (removed in Step 7) ---

        /** @brief Shader compiler instance. */
        rhi::ShaderCompiler shader_compiler_;

        /** @brief Triangle graphics pipeline. */
        rhi::Pipeline triangle_pipeline_;

        /** @brief Triangle vertex buffer handle. */
        rhi::BufferHandle vertex_buffer_;

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
    };

} // namespace himalaya::app
