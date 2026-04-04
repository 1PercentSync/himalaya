/**
 * @file denoiser.cpp
 * @brief OIDN-based asynchronous denoiser implementation.
 */

#include <himalaya/framework/denoiser.h>
#include <himalaya/rhi/context.h>
#include <himalaya/rhi/resources.h>

#include <OpenImageDenoise/oidn.hpp>
#include <spdlog/spdlog.h>

namespace himalaya::framework {
    void Denoiser::init(rhi::Context & /*ctx*/, rhi::ResourceManager & /*rm*/,
                        const uint32_t /*width*/, const uint32_t /*height*/) {
        // TODO: Step 9 — Denoiser::init()
    }

    void Denoiser::request_denoise(const uint32_t /*accumulation_generation*/) {
        // TODO: Step 9 — Denoiser::request_denoise()
    }

    void Denoiser::launch_processing() {
        // TODO: Step 9 — Denoiser::launch_processing()
    }

    bool Denoiser::poll_upload_ready(const uint32_t /*current_generation*/) {
        // TODO: Step 9 — Denoiser::poll_upload_ready()
        return false;
    }

    void Denoiser::complete_upload() {
        // TODO: Step 9 — Denoiser::complete_upload()
    }

    DenoiseState Denoiser::state() const {
        return state_.load(std::memory_order_acquire);
    }

    void Denoiser::on_resize(rhi::ResourceManager & /*rm*/,
                             const uint32_t /*width*/, const uint32_t /*height*/) {
        // TODO: Step 9 — Denoiser::on_resize()
    }

    void Denoiser::abort() {
        join_and_idle();
    }

    void Denoiser::destroy() {
        // TODO: Step 9 — Denoiser::destroy()
    }

    Denoiser::SemaphoreSignal Denoiser::pending_denoise_signal() const {
        if (pending_signal_value_ != 0 &&
            state_.load(std::memory_order_acquire) == DenoiseState::ReadbackPending) {
            return {timeline_semaphore_, pending_signal_value_};
        }
        return {};
    }

    rhi::BufferHandle Denoiser::readback_beauty_buffer() const { return readback_beauty_; }
    rhi::BufferHandle Denoiser::readback_albedo_buffer() const { return readback_albedo_; }
    rhi::BufferHandle Denoiser::readback_normal_buffer() const { return readback_normal_; }
    rhi::BufferHandle Denoiser::upload_buffer() const { return upload_; }
    uint32_t Denoiser::width() const { return width_; }
    uint32_t Denoiser::height() const { return height_; }

    void Denoiser::join_and_idle() {
        thread_ = {};
        state_.store(DenoiseState::Idle, std::memory_order_release);
        pending_signal_value_ = 0;
    }

    void Denoiser::create_staging_buffers(rhi::ResourceManager & /*rm*/,
                                          const uint32_t /*w*/, const uint32_t /*h*/) {
        // TODO: Step 9 — create_staging_buffers()
    }

    void Denoiser::destroy_staging_buffers(rhi::ResourceManager & /*rm*/) {
        // TODO: Step 9 — destroy_staging_buffers()
    }
} // namespace himalaya::framework
