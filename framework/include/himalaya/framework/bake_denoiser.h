#pragma once

/**
 * @file bake_denoiser.h
 * @brief Synchronous OIDN denoiser for offline baking.
 */

#include <cstdint>
#include <memory>

namespace himalaya::framework {

    /**
     * @brief Synchronous OIDN denoiser for offline baking.
     *
     * Unlike the async Denoiser (reference view), BakeDenoiser runs
     * synchronously: readback → OIDN execute → return denoised data.
     * No state machine, no background thread, no timeline semaphore.
     *
     * OIDNBuffer sizes are unknown at init() time. Buffers are lazily
     * created on the first denoise() call based on w × h. Subsequent
     * calls reuse existing buffers if dimensions match, or recreate
     * them if dimensions change.
     */
    class BakeDenoiser {
    public:
        BakeDenoiser();

        ~BakeDenoiser();

        BakeDenoiser(const BakeDenoiser &) = delete;

        BakeDenoiser &operator=(const BakeDenoiser &) = delete;

        /**
         * @brief Creates OIDN device (GPU preferred, CPU fallback) and RT filter.
         *
         * Configures the filter for HDR input with clean auxiliary channels.
         * Does not allocate OIDNBuffers — those are lazily created in denoise().
         */
        void init();

        /**
         * @brief Denoise a single HDR image with albedo/normal auxiliary channels.
         *
         * All buffers are CPU memory (readback results). Output written to
         * caller-provided buffer. Blocking call.
         *
         * @param beauty Input RGBA32F pixel data (w × h × 16 bytes).
         * @param albedo Auxiliary albedo R16G16B16A16F (w × h × 8 bytes, nullable).
         * @param normal Auxiliary normal R16G16B16A16F (w × h × 8 bytes, nullable).
         * @param output Output RGBA32F pixel data (w × h × 16 bytes, caller-allocated).
         * @param w      Image width in pixels.
         * @param h      Image height in pixels.
         * @return true on success, false on OIDN error (logged).
         */
        bool denoise(const void *beauty, const void *albedo, const void *normal,
                     void *output, uint32_t w, uint32_t h);

        /** @brief Releases all OIDN resources (device, filter, buffers). */
        void destroy();

    private:
        /**
         * @brief Ensures OIDNBuffers match the requested dimensions.
         *
         * Creates buffers on first call; recreates if dimensions change.
         */
        void ensure_buffers(uint32_t w, uint32_t h);

        /** @brief PIMPL holding all OIDN objects, hidden from the header. */
        struct OidnImpl;

        /** @brief OIDN device, filter, and buffers. */
        std::unique_ptr<OidnImpl> oidn_;

        /** @brief Cached buffer width (0 = no buffers allocated yet). */
        uint32_t buf_width_ = 0;

        /** @brief Cached buffer height (0 = no buffers allocated yet). */
        uint32_t buf_height_ = 0;
    };

} // namespace himalaya::framework
