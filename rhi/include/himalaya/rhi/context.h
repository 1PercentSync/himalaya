#pragma once

/**
 * @file context.h
 * @brief Vulkan context: instance, device, queues, and memory allocator.
 */

#include <array>
#include <functional>
#include <string>
#include <vector>

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>
#include <spdlog/spdlog.h>

struct GLFWwindow;

/**
 * @brief Checks a VkResult and aborts on failure with diagnostic output.
 *
 * Logs the failed expression, VkResult code, and source location via spdlog.
 * Vulkan API errors during development are programming errors
 * and do not need runtime recovery.
 */
#define VK_CHECK(x)                                                     \
    do {                                                                \
        VkResult vk_check_result_ = (x);                                \
        if (vk_check_result_ != VK_SUCCESS) {                           \
            spdlog::critical("VK_CHECK failed: {} returned {} at {}:{}", \
                             #x,                                        \
                             static_cast<int>(vk_check_result_),        \
                             __FILE__,                                  \
                             __LINE__);                                 \
            std::abort();                                               \
        }                                                               \
    } while (0)

namespace himalaya::rhi {
    /** @brief Number of frames that can be in-flight simultaneously. */
    constexpr uint32_t kMaxFramesInFlight = 2;

    /**
     * @brief Device-local VRAM usage snapshot.
     *
     * Aggregates all device-local memory heaps.
     * Requires VK_EXT_memory_budget (enabled as a device extension).
     */
    struct VramInfo {
        /** @brief Bytes currently used by the application. */
        uint64_t used = 0;

        /** @brief Bytes available to the application (driver-reported budget). */
        uint64_t budget = 0;
    };

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
        void push(std::function<void()> &&fn) { deletors_.push_back(std::move(fn)); }

        /** @brief Executes all queued destructors and clears the queue. */
        void flush() {
            for (auto &fn: deletors_) fn();
            deletors_.clear();
        }

    private:
        /** @brief Queued destruction callables. */
        std::vector<std::function<void()> > deletors_;
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

        /** @brief Human-readable GPU name, populated during init. */
        std::string gpu_name;

        /** @brief Device-supported maximum sampler anisotropy (e.g. 16.0f). */
        float max_sampler_anisotropy;

        /**
         * @brief MSAA sample count bitmask supported by both color and depth framebuffers.
         *
         * Each set bit corresponds to a VkSampleCountFlagBits value (1, 2, 4, 8, ...).
         * Use `(msaa_sample_counts & VK_SAMPLE_COUNT_4_BIT)` to test specific support.
         */
        VkSampleCountFlags msaa_sample_counts;

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

        /**
         * @brief Command pool for immediate one-shot GPU operations.
         *
         * Used by upload_buffer and similar blocking transfer operations.
         * Decoupled from per-frame command pools so uploads are safe to call
         * at any point (init-time or mid-frame) without conflicting with
         * the frame's primary command buffer.
         */
        VkCommandPool immediate_command_pool = VK_NULL_HANDLE;

        /** @brief Command buffer for immediate one-shot GPU operations. */
        VkCommandBuffer immediate_command_buffer = VK_NULL_HANDLE;

        /** @brief Returns the FrameData for the current in-flight frame. */
        FrameData &current_frame() { return frames[frame_index]; }

        /** @brief Advances to the next in-flight frame index. */
        void advance_frame() { frame_index = (frame_index + 1) % kMaxFramesInFlight; }

        /**
         * @brief Queries aggregated device-local VRAM usage from VMA.
         * @return Snapshot of current VRAM usage and budget.
         */
        [[nodiscard]] VramInfo query_vram_usage() const;

        // --- Immediate command scope ---

        /**
         * @brief Begins an immediate command recording scope.
         *
         * Resets and begins the immediate command buffer. Resource upload methods
         * (upload_buffer, upload_image, generate_mips) must only be called within
         * an active immediate scope. Staging buffers created by upload methods are
         * collected and destroyed when end_immediate() completes.
         */
        void begin_immediate();

        /**
         * @brief Ends the immediate scope: submits commands and waits for completion.
         *
         * Ends the command buffer, submits it to the graphics queue, waits for
         * the GPU to finish (vkQueueWaitIdle), and destroys all staging buffers
         * collected during the scope.
         */
        void end_immediate();

        /**
         * @brief Returns whether an immediate command scope is currently active.
         */
        [[nodiscard]] bool is_immediate_active() const { return immediate_active_; }

        /**
         * @brief Registers a staging buffer for deferred cleanup at end_immediate().
         *
         * Called by ResourceManager upload methods to defer staging buffer
         * destruction until after the GPU has finished executing.
         *
         * @param buffer     Staging VkBuffer handle.
         * @param allocation VMA allocation backing the staging buffer.
         */
        void push_staging_buffer(VkBuffer buffer, VmaAllocation allocation);

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

        /** @brief Creates the immediate command pool for one-shot blocking GPU operations. */
        void create_immediate_pool();

        /** @brief Whether an immediate command scope is currently active. */
        bool immediate_active_ = false;

        /** @brief Staging buffer pending cleanup at end_immediate(). */
        struct StagingEntry {
            VkBuffer buffer;
            VmaAllocation allocation;
        };

        /** @brief Staging buffers collected during the current immediate scope. */
        std::vector<StagingEntry> staging_buffers_;
    };
} // namespace himalaya::rhi
