#pragma once

/**
 * @file imgui_backend.h
 * @brief ImGui integration for the Himalaya renderer.
 */

#include <vulkan/vulkan.h>

struct GLFWwindow;

namespace himalaya::rhi {
    class Context;
    class Swapchain;
} // namespace himalaya::rhi

namespace himalaya::framework {
    /**
     * @brief Manages the ImGui lifecycle: context, platform backend, and renderer backend.
     *
     * Initializes ImGui with Vulkan Dynamic Rendering (no VkRenderPass) and the
     * GLFW platform backend. Owns a dedicated descriptor pool for ImGui's internal
     * texture descriptors.
     *
     * Lifetime is managed explicitly via init() and destroy().
     * destroy() must be called before the Vulkan device is destroyed.
     */
    class ImGuiBackend {
    public:
        /**
         * @brief Initializes ImGui context, GLFW backend, and Vulkan backend.
         *
         * Creates a dedicated descriptor pool, sets up GLFW input handling,
         * and configures the Vulkan backend for Dynamic Rendering with the
         * swapchain's color format.
         *
         * @param context   Vulkan context providing device, queues, and instance.
         * @param swapchain Swapchain providing format and image count.
         * @param window    GLFW window for input handling.
         */
        void init(const rhi::Context &context, const rhi::Swapchain &swapchain, GLFWwindow *window);

        /**
         * @brief Shuts down ImGui backends and destroys the descriptor pool.
         *
         * Must be called after the graphics queue is idle to ensure no GPU work
         * references ImGui resources.
         */
        void destroy();

        /**
         * @brief Begins a new ImGui frame.
         *
         * Calls NewFrame on both the Vulkan and GLFW backends, then ImGui::NewFrame().
         * Must be called once per frame before any ImGui draw commands.
         */
        void begin_frame();

        /**
         * @brief Finalizes ImGui rendering and records draw commands.
         *
         * Calls ImGui::Render() and records the draw data into the given command buffer.
         * Must be called within an active dynamic rendering pass.
         *
         * @param cmd Command buffer to record ImGui draw commands into.
         */
        void render(VkCommandBuffer cmd);

    private:
        /** @brief Dedicated descriptor pool for ImGui's internal textures. */
        VkDescriptorPool descriptor_pool_ = VK_NULL_HANDLE;

        /** @brief Cached device handle for cleanup. */
        VkDevice device_ = VK_NULL_HANDLE;
    };
} // namespace himalaya::framework
