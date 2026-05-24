#pragma once

/**
 * @file gs_tile_buffers.h
 * @brief Gaussian Splatting tile-binning buffer storage.
 */

#include <himalaya/rhi/resources.h>
#include <himalaya/rhi/types.h>

#include <algorithm>
#include <cstdint>

#include <spdlog/spdlog.h>

namespace himalaya::passes {
    /**
     * @brief Owns GPU buffers produced and consumed by the GS tile-binning stage.
     *
     * The tile count, offset, and cursor buffers are sized by the current render
     * target tile grid. The splat-id list uses a conservative fixed upper bound
     * based on the maximum visible splat capacity, avoiding per-frame GPU result
     * readback or dynamic buffer reallocation.
     */
    class GsTileBuffers {
    public:
        /** @brief Tile width and height in pixels. Must match GS shaders. */
        static constexpr uint32_t kTileSize = 16;

        /** @brief Conservative maximum number of tiles covered by one splat. */
        static constexpr uint32_t kMaxTilesPerSplat = 64;

        /**
         * @brief Stores the resource manager used for all buffer operations.
         */
        void setup(rhi::ResourceManager &rm) {
            rm_ = &rm;
        }

        /**
         * @brief Ensures all tile-binning buffers match the requested capacity.
         *
         * Passing zero dimensions or zero splat capacity destroys existing
         * buffers. Buffers are recreated whenever tile grid dimensions or maximum
         * splat capacity change.
         */
        void ensure_capacity(const uint32_t max_splat_count,
                             const uint32_t screen_width,
                             const uint32_t screen_height) {
            const uint32_t next_tile_count_x = ceil_div(screen_width, kTileSize);
            const uint32_t next_tile_count_y = ceil_div(screen_height, kTileSize);
            const uint32_t next_tile_count = next_tile_count_x * next_tile_count_y;
            const uint32_t next_splat_id_capacity = max_splat_count * kMaxTilesPerSplat;

            if (max_splat_count == max_splat_count_ &&
                next_tile_count_x == tile_count_x_ &&
                next_tile_count_y == tile_count_y_) {
                return;
            }

            destroy();

            max_splat_count_ = max_splat_count;
            tile_count_x_ = next_tile_count_x;
            tile_count_y_ = next_tile_count_y;
            tile_count_ = next_tile_count;
            tile_splat_id_capacity_ = next_splat_id_capacity;
            scan_chunk_count_ = ceil_div(tile_count_, kTileSize);

            if (max_splat_count_ == 0 || tile_count_ == 0 || tile_splat_id_capacity_ == 0) {
                return;
            }

            const uint64_t tile_buffer_size = static_cast<uint64_t>(tile_count_) * sizeof(uint32_t);
            const uint64_t chunk_sum_buffer_size =
                static_cast<uint64_t>(std::max(scan_chunk_count_, 1u)) * sizeof(uint32_t);
            const uint64_t tile_splat_id_buffer_size =
                static_cast<uint64_t>(tile_splat_id_capacity_) * sizeof(uint32_t);

            const rhi::BufferDesc tile_buffer_desc{
                .size = tile_buffer_size,
                .usage = rhi::BufferUsage::StorageBuffer | rhi::BufferUsage::TransferDst,
                .memory = rhi::MemoryUsage::GpuOnly,
            };
            tile_offsets_buffer_ = rm_->create_buffer(tile_buffer_desc, "GS Tile Offsets SSBO");
            tile_counts_buffer_ = rm_->create_buffer(tile_buffer_desc, "GS Tile Counts SSBO");
            tile_cursors_buffer_ = rm_->create_buffer(tile_buffer_desc, "GS Tile Cursors SSBO");

            tile_scan_chunk_sums_buffer_ = rm_->create_buffer({
                .size = chunk_sum_buffer_size,
                .usage = rhi::BufferUsage::StorageBuffer | rhi::BufferUsage::TransferDst,
                .memory = rhi::MemoryUsage::GpuOnly,
            }, "GS Tile Scan Chunk Sums SSBO");

            tile_total_count_buffer_ = rm_->create_buffer({
                .size = sizeof(uint32_t),
                .usage = rhi::BufferUsage::StorageBuffer | rhi::BufferUsage::TransferDst,
                .memory = rhi::MemoryUsage::GpuOnly,
            }, "GS Tile Total Count SSBO");

            tile_splat_ids_buffer_ = rm_->create_buffer({
                .size = tile_splat_id_buffer_size,
                .usage = rhi::BufferUsage::StorageBuffer,
                .memory = rhi::MemoryUsage::GpuOnly,
            }, "GS Tile Splat IDs SSBO");

            spdlog::info("GsTileBuffers: allocated {} tiles and {} tile-splat ID slots",
                         tile_count_, tile_splat_id_capacity_);
        }

        /**
         * @brief Destroys all owned tile-binning buffers and resets capacity.
         */
        void destroy() {
            if (rm_ == nullptr) {
                reset_capacity();
                return;
            }

            destroy_buffer(tile_offsets_buffer_);
            destroy_buffer(tile_counts_buffer_);
            destroy_buffer(tile_cursors_buffer_);
            destroy_buffer(tile_scan_chunk_sums_buffer_);
            destroy_buffer(tile_total_count_buffer_);
            destroy_buffer(tile_splat_ids_buffer_);
            reset_capacity();
        }

        /** @brief Buffer containing exclusive per-tile offsets. */
        [[nodiscard]] rhi::BufferHandle tile_offsets_buffer() const {
            return tile_offsets_buffer_;
        }

        /** @brief Buffer containing per-tile splat counts. */
        [[nodiscard]] rhi::BufferHandle tile_counts_buffer() const {
            return tile_counts_buffer_;
        }

        /** @brief Buffer containing per-tile scatter write cursors. */
        [[nodiscard]] rhi::BufferHandle tile_cursors_buffer() const {
            return tile_cursors_buffer_;
        }

        /** @brief Buffer containing tile-scan per-chunk sums. */
        [[nodiscard]] rhi::BufferHandle tile_scan_chunk_sums_buffer() const {
            return tile_scan_chunk_sums_buffer_;
        }

        /** @brief Buffer containing the exact tile-list entry count written by scan. */
        [[nodiscard]] rhi::BufferHandle tile_total_count_buffer() const {
            return tile_total_count_buffer_;
        }

        /** @brief Buffer containing concatenated per-tile splat index lists. */
        [[nodiscard]] rhi::BufferHandle tile_splat_ids_buffer() const {
            return tile_splat_ids_buffer_;
        }

        /** @brief Number of tiles along the current render target X axis. */
        [[nodiscard]] uint32_t tile_count_x() const {
            return tile_count_x_;
        }

        /** @brief Number of tiles along the current render target Y axis. */
        [[nodiscard]] uint32_t tile_count_y() const {
            return tile_count_y_;
        }

        /** @brief Total number of tiles in the current render target. */
        [[nodiscard]] uint32_t tile_count() const {
            return tile_count_;
        }

        /** @brief Number of 256-tile chunks used by gs_tile_scan.comp. */
        [[nodiscard]] uint32_t scan_chunk_count() const {
            return scan_chunk_count_;
        }

        /** @brief Maximum number of visible splats represented by this allocation. */
        [[nodiscard]] uint32_t max_splat_count() const {
            return max_splat_count_;
        }

        /** @brief Capacity of tile_splat_ids_buffer() in uint32 entries. */
        [[nodiscard]] uint32_t tile_splat_id_capacity() const {
            return tile_splat_id_capacity_;
        }

    private:
        /**
         * @brief Integer ceil division helper.
         */
        static constexpr uint32_t ceil_div(const uint32_t value, const uint32_t divisor) {
            return divisor == 0 ? 0 : (value + divisor - 1u) / divisor;
        }

        /**
         * @brief Destroys one valid buffer and clears its handle.
         */
        void destroy_buffer(rhi::BufferHandle &buffer) {
            if (buffer.valid()) {
                rm_->destroy_buffer(buffer);
                buffer = {};
            }
        }

        /**
         * @brief Resets capacity counters without touching GPU resources.
         */
        void reset_capacity() {
            max_splat_count_ = 0;
            tile_count_x_ = 0;
            tile_count_y_ = 0;
            tile_count_ = 0;
            scan_chunk_count_ = 0;
            tile_splat_id_capacity_ = 0;
        }

        /** @brief Resource manager used to create and destroy owned buffers. */
        rhi::ResourceManager *rm_ = nullptr;

        /** @brief Buffer containing exclusive per-tile offsets. */
        rhi::BufferHandle tile_offsets_buffer_;

        /** @brief Buffer containing per-tile splat counts. */
        rhi::BufferHandle tile_counts_buffer_;

        /** @brief Buffer containing per-tile scatter write cursors. */
        rhi::BufferHandle tile_cursors_buffer_;

        /** @brief Buffer containing per-chunk sums for tile scan. */
        rhi::BufferHandle tile_scan_chunk_sums_buffer_;

        /** @brief Buffer containing exact total tile-list length after scan. */
        rhi::BufferHandle tile_total_count_buffer_;

        /** @brief Buffer containing concatenated per-tile splat indices. */
        rhi::BufferHandle tile_splat_ids_buffer_;

        /** @brief Maximum visible splat capacity. */
        uint32_t max_splat_count_ = 0;

        /** @brief Number of tiles along X. */
        uint32_t tile_count_x_ = 0;

        /** @brief Number of tiles along Y. */
        uint32_t tile_count_y_ = 0;

        /** @brief Total tile count. */
        uint32_t tile_count_ = 0;

        /** @brief Number of 256-tile scan chunks. */
        uint32_t scan_chunk_count_ = 0;

        /** @brief Capacity of tile_splat_ids_buffer_ in uint32 entries. */
        uint32_t tile_splat_id_capacity_ = 0;
    };
} // namespace himalaya::passes
