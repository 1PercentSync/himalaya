#pragma once

/**
 * @file denoiser.h
 * @brief OIDN-based asynchronous denoiser for path tracing output.
 */

#include <himalaya/rhi/resources.h>

#include <atomic>
#include <thread>
#include <vulkan/vulkan.h>

namespace oidn {
    class DeviceRef;
    class FilterRef;
    class BufferRef;
} // namespace oidn

namespace himalaya::rhi {
    class Context;
    class ResourceManager;
} // namespace himalaya::rhi

namespace himalaya::framework {
    /** @brief Denoise pipeline state machine. */
    enum class DenoiseState : uint8_t {
        /** @brief No denoise in progress. Ready to accept new requests. */
        Idle,

        /** @brief Denoise requested. Caller must register readback copy pass this frame. */
        ReadbackPending,

        /** @brief Background thread running oidnExecuteFilter. */
        Processing,

        /** @brief Filter complete. Caller must register upload pass. */
        UploadPending,
    };

    /**
     * @brief Asynchronous OIDN denoiser with zero main-thread blocking.
     *
     * Workflow:
     *  1. request_denoise() → state becomes ReadbackPending
     *  2. Caller registers RG readback copy pass (image → staging buffer)
     *  3. launch_processing() → spawns jthread, state becomes Processing
     *     Thread: vkWaitSemaphores → memcpy staging→OIDN → filter → memcpy OIDN→staging
     *  4. poll_upload_ready() → returns true when Processing finishes
     *  5. Caller registers RG upload pass (staging → denoised image)
     *  6. complete_upload() → state returns to Idle
     *
     * Timeline semaphore synchronization: the denoiser owns a timeline
     * semaphore that the render submit signals; the background thread
     * waits on it to know readback is complete.
     *
     * Generation tracking: each denoise request records the accumulation
     * generation. If the generation changes before upload (camera move,
     * resize, etc.), the result is discarded.
     */
    class Denoiser {
    public:
        /**
         * @brief Initializes OIDN device, filter, staging buffers, and timeline semaphore.
         *
         * Creates OIDN device with default type (GPU preferred, CPU fallback).
         * Logs actual device type: GPU → info, CPU → warn (~25x slower).
         * Allocates persistent staging buffers for readback and upload.
         *
         * @param ctx Vulkan context for device handle and semaphore creation.
         * @param rm  Resource manager for staging buffer allocation.
         * @param width  Image width in pixels.
         * @param height Image height in pixels.
         */
        void init(const rhi::Context &ctx, rhi::ResourceManager &rm,
                  uint32_t width, uint32_t height);

        /**
         * @brief Requests a denoise operation.
         *
         * Records the accumulation generation for staleness detection.
         * State transitions to ReadbackPending. Caller must register
         * a readback copy pass in the same frame's render graph.
         *
         * @pre state() == Idle
         * @param accumulation_generation Current accumulation generation counter.
         */
        void request_denoise(uint32_t accumulation_generation);

        /**
         * @brief Launches the background denoise thread.
         *
         * Spawns a jthread that waits on the timeline semaphore, copies
         * data to OIDN buffers, executes the filter, copies results back,
         * and transitions state to UploadPending (or Idle on failure).
         *
         * @pre state() == ReadbackPending (readback pass has been recorded)
         */
        void launch_processing();

        /**
         * @brief Checks whether denoised output is ready for upload.
         *
         * If state is UploadPending and generation matches, returns true.
         * If generation mismatches (accumulation was reset), discards the
         * result and returns to Idle.
         *
         * @param current_generation Current accumulation generation counter.
         * @return true if upload pass should be registered this frame.
         */
        [[nodiscard]] bool poll_upload_ready(uint32_t current_generation);

        /**
         * @brief Marks the upload as complete.
         *
         * Called after the upload pass has executed. State returns to Idle.
         *
         * @pre state() == UploadPending
         */
        void complete_upload();

        /** @brief Returns the current denoise state (acquire semantics). */
        [[nodiscard]] DenoiseState state() const;

        /**
         * @brief Handles window resize: joins thread, resets state, rebuilds staging buffers.
         *
         * @param rm     Resource manager for buffer reallocation.
         * @param width  New image width in pixels.
         * @param height New image height in pixels.
         */
        void on_resize(rhi::ResourceManager &rm, uint32_t width, uint32_t height);

        /**
         * @brief Aborts any in-progress denoise (joins thread, resets to Idle).
         *
         * Called before scene loading to ensure clean state.
         * Does not release resources — only cancels pending work.
         */
        void abort();

        /**
         * @brief Destroys all resources: OIDN device/filter, staging buffers, semaphore.
         *
         * Joins background thread first if running.
         */
        void destroy();

        /**
         * @brief Returns timeline semaphore + signal value for the current denoise frame.
         *
         * The Application injects this into vkQueueSubmit2 signalSemaphoreInfos.
         * Returns {VK_NULL_HANDLE, 0} when no signal is needed.
         * One-shot: clears the pending value after returning, ensuring the
         * semaphore is signaled exactly once per denoise request.
         */
        struct SemaphoreSignal {
            VkSemaphore semaphore = VK_NULL_HANDLE;
            uint64_t value = 0;
        };
        SemaphoreSignal pending_denoise_signal();

        /** @brief Returns the readback staging buffer for beauty (RGBA32F). */
        [[nodiscard]] rhi::BufferHandle readback_beauty_buffer() const;

        /** @brief Returns the readback staging buffer for aux albedo (RGBA16F). */
        [[nodiscard]] rhi::BufferHandle readback_albedo_buffer() const;

        /** @brief Returns the readback staging buffer for aux normal (RGBA16F). */
        [[nodiscard]] rhi::BufferHandle readback_normal_buffer() const;

        /** @brief Returns the upload staging buffer for denoised output (RGBA32F). */
        [[nodiscard]] rhi::BufferHandle upload_buffer() const;

        /** @brief Image width used by staging buffers. */
        [[nodiscard]] uint32_t width() const;

        /** @brief Image height used by staging buffers. */
        [[nodiscard]] uint32_t height() const;

    private:
        /** @brief Joins the background thread if joinable and forces state to Idle. */
        void join_and_idle();

        /** @brief Allocates persistent staging buffers for readback and upload. */
        void create_staging_buffers(rhi::ResourceManager &rm, uint32_t w, uint32_t h);

        /** @brief Destroys staging buffers. */
        void destroy_staging_buffers(rhi::ResourceManager &rm);

        // ---- Vulkan resources ----
        VkDevice device_ = VK_NULL_HANDLE;
        VkSemaphore timeline_semaphore_ = VK_NULL_HANDLE;
        uint64_t semaphore_value_ = 0;

        // ---- OIDN resources (opaque pointers to avoid header leak) ----
        /** @brief OIDN device handle. Managed via oidn.hpp C++ wrapper. */
        void *oidn_device_ = nullptr;

        /** @brief OIDN "RT" filter handle. */
        void *oidn_filter_ = nullptr;

        /** @brief OIDN buffer for beauty input (RGBA32F). */
        void *oidn_beauty_buf_ = nullptr;

        /** @brief OIDN buffer for albedo auxiliary input (RGBA16F). */
        void *oidn_albedo_buf_ = nullptr;

        /** @brief OIDN buffer for normal auxiliary input (RGBA16F). */
        void *oidn_normal_buf_ = nullptr;

        /** @brief OIDN buffer for denoised output (RGBA32F). */
        void *oidn_output_buf_ = nullptr;

        // ---- Vulkan staging buffers ----
        rhi::BufferHandle readback_beauty_{};
        rhi::BufferHandle readback_albedo_{};
        rhi::BufferHandle readback_normal_{};
        rhi::BufferHandle upload_{};

        // ---- Resource manager (for destroy) ----
        rhi::ResourceManager *rm_ = nullptr;

        // ---- Dimensions ----
        uint32_t width_ = 0;
        uint32_t height_ = 0;

        // ---- State machine ----
        std::atomic<DenoiseState> state_{DenoiseState::Idle};
        uint32_t trigger_generation_ = 0;
        uint64_t pending_signal_value_ = 0;

        // ---- Background thread ----
        std::jthread thread_;
    };
} // namespace himalaya::framework
