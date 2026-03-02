#pragma once

/**
 * @file context.h
 * @brief Vulkan context: instance, device, queues, and memory allocator.
 */

#include <array>
#include <functional>
#include <vector>

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

    /** @brief Number of frames that can be in-flight simultaneously. */
    constexpr uint32_t kMaxFramesInFlight = 2;

    /**
     * @brief Deferred resource destruction queue.
     *
     * Resources cannot be destroyed immediately because the GPU may still
     * reference them. Instead, destruction lambdas are pushed into the queue
     * and flushed once the corresponding frame's fence has been waited on.
     */
    struct DeletionQueue {
        /**
         * @brief Enqueues a destructor to be called later.
         * @param fn Callable that destroys one or more Vulkan resources.
         */
        void push(std::function<void()> &&fn) { deletors.push_back(std::move(fn)); }

        /** @brief Executes all queued destructors and clears the queue. */
        void flush() {
            for (auto &fn : deletors) fn();
            deletors.clear();
        }

    private:
        /** @brief Queued destruction callables. */
        std::vector<std::function<void()>> deletors;
    };

    /**
     * @brief Per-frame GPU synchronization and command recording resources.
     *
     * Each in-flight frame owns an independent set of these objects
     * so the CPU can record frame N+1 while the GPU is still executing frame N.
     */
    struct FrameData {
        /** @brief Command pool for this frame's command buffer allocation. */
        VkCommandPool command_pool = VK_NULL_HANDLE;

        /** @brief Primary command buffer recorded each frame. */
        VkCommandBuffer command_buffer = VK_NULL_HANDLE;

        /** @brief Signaled by the GPU when this frame's commands finish executing. */
        VkFence render_fence = VK_NULL_HANDLE;

        /** @brief Signaled when a swapchain image has been acquired for this frame. */
        VkSemaphore image_available_semaphore = VK_NULL_HANDLE;

        /** @brief Deferred deletions executed after the fence confirms GPU completion. */
        DeletionQueue deletion_queue;
    };

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
        void destroy();

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

        /** @brief Per-frame synchronization and command recording resources. */
        std::array<FrameData, kMaxFramesInFlight> frames{};

        /** @brief Index of the current in-flight frame (0 to kMaxFramesInFlight-1). */
        uint32_t frame_index = 0;

        /** @brief Returns the FrameData for the current in-flight frame. */
        FrameData &current_frame() { return frames[frame_index]; }

        /** @brief Advances to the next in-flight frame index. */
        void advance_frame() { frame_index = (frame_index + 1) % kMaxFramesInFlight; }

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

        /** @brief Creates per-frame command pools, command buffers, fences, and semaphores. */
        void create_frame_data();
    };
} // namespace himalaya::rhi
