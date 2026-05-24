#pragma once

/**
 * @file gs_gpu_data.h
 * @brief Gaussian Splatting GPU buffer management (Framework layer).
 *
 * Owns the GPU SSBOs for GS scene data: a merged core attributes buffer
 * and per-max-SH-degree coefficient buffers. Upload happens once at scene
 * load time via staging; destroy() releases all GPU resources.
 */

#include <himalaya/framework/gaussian_splat_data.h>
#include <himalaya/rhi/types.h>

#include <array>
#include <cstdint>
#include <vector>

namespace himalaya::rhi {
    class ResourceManager;
}

namespace himalaya::framework {
    /**
     * @brief Manages GPU buffers for Gaussian Splatting scene data.
     *
     * Owns the core attributes SSBO (merged from all primitives, positions
     * transformed to world space) and per-max-SH-degree SSBOs (cumulative
     * coefficients up to the declared degree).
     *
     * Also records dispatch grouping metadata so the projection pass can
     * issue one dispatch per (sh_degree, splat range) group.
     *
     * Lifetime:
     *   init(rm) → upload(scene) → [rendering...] → destroy()
     *
     * Upload must be called within a Context::begin_immediate() /
     * end_immediate() scope because it uses staging-buffer uploads.
     */
    class GsGpuData {
    public:
        /**
         * @brief Dispatch group descriptor for the projection pass.
         *
         * Each group corresponds to all splats in the scene that share
         * the same max_sh_degree. The projection pass dispatches one
         * workgroup range per group, reading the appropriate SH buffer
         * via push constant (splat_offset, splat_count, sh_degree).
         */
        struct ShGroup {
            uint32_t splat_offset; ///< Splat index offset into the core buffer (also used to index the SH buffer).
            uint32_t splat_count;  ///< Number of splats in this group.
            uint32_t sh_degree;    ///< Max SH degree for this group (0-3). Determines SH buffer stride.
        };

        /**
         * @brief Stores the resource manager pointer.
         * @param rm Resource manager for buffer creation and upload.
         */
        void init(rhi::ResourceManager *rm);

        /**
         * @brief Uploads GS scene data to GPU.
         *
         * Allocates GpuOnly SSBOs and uploads via staging. Merges core
         * attributes from all primitives, applying each primitive's node
         * transform to positions. Groups SH coefficients by max_sh_degree
         * with cumulative coefficient layout.
         *
         * Must be called within begin_immediate() / end_immediate() scope.
         * Safe to call multiple times (destroys previous buffers first).
         *
         * @param scene The loaded GS scene data.
         */
        void upload(const GaussianSplatScene &scene);

        /**
         * @brief Destroys all GPU buffers and resets metadata.
         */
        void destroy();

        // ---- Accessors ----

        /** @brief Handle to the merged core attributes SSBO. */
        [[nodiscard]] rhi::BufferHandle core_buffer() const;

        /**
         * @brief Handle to the SH coefficient SSBO for the given max SH degree.
         * @param max_sh_degree Max SH degree (0-3).
         * @return Valid BufferHandle if that degree group exists, invalid otherwise.
         */
        [[nodiscard]] rhi::BufferHandle sh_buffer(uint32_t max_sh_degree) const;

        /** @brief Dispatch groups for the projection pass. */
        [[nodiscard]] const std::vector<ShGroup> &sh_groups() const;

        /** @brief Total number of splats across all primitives. */
        [[nodiscard]] uint32_t total_splat_count() const;

    private:
        /** @brief Resource manager (non-owning). */
        rhi::ResourceManager *rm_ = nullptr;

        /** @brief Merged core attributes buffer (StorageBuffer | TransferDst, GpuOnly). */
        rhi::BufferHandle core_buffer_;

        /**
         * @brief SH coefficient buffers per max SH degree (0-3).
         *
         * Each buffer contains cumulative coefficients for all splats with
         * that max_sh_degree. Stride per splat:
         *   degree 0:  1 vec3 =  12 bytes
         *   degree 1:  4 vec3 =  48 bytes
         *   degree 2:  9 vec3 = 108 bytes
         *   degree 3: 16 vec3 = 192 bytes
         */
        std::array<rhi::BufferHandle, 4> sh_buffers_{};

        /** @brief Per-max-SH-degree dispatch groups. */
        std::vector<ShGroup> sh_groups_;

        /** @brief Total splat count across all groups. */
        uint32_t total_splat_count_ = 0;
    };
} // namespace himalaya::framework
