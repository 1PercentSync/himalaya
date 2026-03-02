#pragma once

/**
 * @file context.h
 * @brief Vulkan context: instance, device, queues, and memory allocator.
 */

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>

struct GLFWwindow;

/**
 * @brief Checks a VkResult and aborts on failure.
 *
 * Vulkan API errors during development are programming errors
 * and do not need runtime recovery.
 */
#define VK_CHECK(x)                                                     \
    do {                                                                \
        VkResult result = (x);                                          \
        if (result != VK_SUCCESS) {                                     \
            std::abort();                                               \
        }                                                               \
    } while (0)

namespace himalaya::rhi {
    /**
     * @brief Core Vulkan context owning instance, device, queues, and allocator.
     *
     * Lifetime is managed explicitly via init() and destroy().
     * destroy() must be called before application exit, in reverse order
     * relative to other RHI objects that depend on this context.
     */
    class Context {
    public:
        /**
         * @brief Initializes the entire Vulkan context.
         * @param window GLFW window used to create the Vulkan surface.
         */
        void init(GLFWwindow *window);

        /**
         * @brief Destroys all Vulkan objects in reverse creation order.
         */
        void destroy() const;

        /** @brief Vulkan instance. */
        VkInstance instance = VK_NULL_HANDLE;

        /** @brief Debug messenger for validation layer callbacks. */
        VkDebugUtilsMessengerEXT debug_messenger = VK_NULL_HANDLE;

        /** @brief Selected physical device (GPU). */
        VkPhysicalDevice physical_device = VK_NULL_HANDLE;

        /** @brief Logical device. */
        VkDevice device = VK_NULL_HANDLE;

        /** @brief Graphics queue (also used for presentation). */
        VkQueue graphics_queue = VK_NULL_HANDLE;

        /** @brief Queue family index for graphics_queue. */
        uint32_t graphics_queue_family = 0;

        /** @brief Window surface for swapchain presentation. */
        VkSurfaceKHR surface = VK_NULL_HANDLE;

        /** @brief VMA allocator for GPU memory management. */
        VmaAllocator allocator = VK_NULL_HANDLE;

    private:
        /** @brief Creates VkInstance with validation layers and debug_utils extension. */
        void create_instance();

        /** @brief Sets up the debug messenger callback for validation messages. */
        void create_debug_messenger();

        /** @brief Selects a suitable physical device, preferring discrete GPUs. */
        void pick_physical_device();

        /** @brief Creates logical device and retrieves the graphics queue. */
        void create_device();

        /** @brief Initializes VMA allocator. */
        void create_allocator();
    };
} // namespace himalaya::rhi
