#pragma once

/**
 * @file application.h
 * @brief Main application class: window management, frame loop, init/destroy sequence.
 */

#include <himalaya/app/camera_controller.h>
#include <himalaya/app/debug_ui.h>
#include <himalaya/app/scene_loader.h>
#include <himalaya/framework/camera.h>
#include <himalaya/framework/imgui_backend.h>
#include <himalaya/framework/render_graph.h>
#include <himalaya/framework/texture.h>
#include <himalaya/rhi/context.h>
#include <himalaya/rhi/descriptors.h>
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
        /**
         * @brief Initializes GLFW, all subsystems, and loads the scene.
         * @param scene_path Path to the glTF scene file.
         */
        void init(const std::string &scene_path);

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
        rhi::Context context_;

        /** @brief Swapchain: presentation surface, images, and image views. */
        rhi::Swapchain swapchain_;

        /** @brief GPU resource pool: buffers, images, and samplers. */
        rhi::ResourceManager resource_manager_;

        /** @brief Descriptor set layouts, pools, and bindless texture management. */
        rhi::DescriptorManager descriptor_manager_;

        // --- Framework ---

        /** @brief ImGui integration backend. */
        framework::ImGuiBackend imgui_backend_;

        /** @brief Render graph for pass orchestration and automatic barriers. */
        framework::RenderGraph render_graph_;

        /** @brief Material SSBO management (Set 0, Binding 2). */
        framework::MaterialSystem material_system_;

        // --- App modules ---

        /** @brief Camera state (position, orientation, matrices). */
        framework::Camera camera_;

        /** @brief Free-roaming camera controller. */
        CameraController camera_controller_;

        /** @brief Debug UI panel. */
        DebugUI debug_ui_;

        /** @brief glTF scene loader and resource owner. */
        SceneLoader scene_loader_;

        /** @brief Per-frame scene render data (populated in update()). */
        framework::SceneRenderData scene_render_data_;

        /** @brief Per-frame frustum culling result (populated in update()). */
        framework::CullResult cull_result_;

        /** @brief Fallback directional light when the scene provides none. */
        std::vector<framework::DirectionalLight> default_lights_;

        // --- Shared resources ---

        /** @brief Depth buffer (D32Sfloat, recreated on resize). */
        rhi::ImageHandle depth_image_;

        /** @brief Default sampler (linear filter, repeat wrap, linear mip). */
        rhi::SamplerHandle default_sampler_;

        /** @brief Default 1x1 textures (white, flat normal, black). */
        framework::DefaultTextures default_textures_;

        /** @brief Unlit graphics pipeline (forward.vert + forward.frag). */
        rhi::Pipeline unlit_pipeline_;

        /** @brief Shader compiler instance. */
        rhi::ShaderCompiler shader_compiler_;

        // --- Per-frame buffers ---

        /** @brief Per-frame GlobalUBO buffers (CpuToGpu, one per frame in flight). */
        std::array<rhi::BufferHandle, rhi::kMaxFramesInFlight> global_ubo_buffers_;

        /** @brief Per-frame LightBuffer SSBOs (CpuToGpu, one per frame in flight). */
        std::array<rhi::BufferHandle, rhi::kMaxFramesInFlight> light_buffers_;

        /** @brief Whether VSync was toggled this frame (triggers swapchain recreate). */
        bool vsync_changed_ = false;

        /** @brief Acquired swapchain image index for the current frame. */
        uint32_t image_index_ = 0;

        /** @brief Registered ImageHandles for swapchain images (one per swapchain image). */
        std::vector<rhi::ImageHandle> swapchain_image_handles_;

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
         * @brief Registers all swapchain images as external images in ResourceManager.
         */
        void register_swapchain_images();

        /**
         * @brief Unregisters all swapchain images from ResourceManager.
         */
        void unregister_swapchain_images();

        /** @brief Creates the depth buffer matching the current swapchain extent. */
        void create_depth_buffer();

        /** @brief Destroys the depth buffer (called before resize recreation). */
        void destroy_depth_buffer();
    };
} // namespace himalaya::app
