#pragma once

/**
 * @file gs_tile_buffers.h
 * @brief Gaussian Splatting tile-entry buffer storage.
 */

#include <himalaya/framework/radix_sort.h>
#include <himalaya/rhi/resources.h>
#include <himalaya/rhi/types.h>

#include <algorithm>
#include <cstdint>
#include <limits>

#include <spdlog/spdlog.h>

namespace himalaya::passes {
    /**
     * @brief Owns GPU buffers produced and consumed by the GS tile-entry pipeline.
     *
     * The tile range buffers are sized by the current render target tile grid.
     * Entry buffers are sized by a bounded capacity strategy so entry generation
     * can safely drop overflow entries instead of writing out of bounds.
     */
    class GsTileBuffers {
    public:
        /** @brief Tile width and height in pixels. Must match GS shaders. */
        static constexpr uint32_t kTileSize = 16;

        /** @brief Average tiles-per-splat budget used for fixed entry capacity. */
        static constexpr uint32_t kAvgTilesBudget = 16;

        /** @brief Absolute entry capacity limit imposed by the current radix-sort scan. */
        static constexpr uint32_t kMaxSortableEntries = framework::RadixSort::kMaxSortableElements;

        /**
         * @brief Stores the resource manager used for all buffer operations.
         */
        void setup(rhi::ResourceManager &rm) {
            rm_ = &rm;
        }

        /**
         * @brief Ensures all tile-entry buffers match the requested capacity.
         *
         * Passing zero dimensions or zero splat capacity destroys existing
         * buffers. Buffers are recreated whenever tile grid dimensions, maximum
         * splat capacity, or derived entry capacity changes.
         */
        void ensure_capacity(const uint32_t max_splat_count,
                             const uint32_t screen_width,
                             const uint32_t screen_height) {
            const uint32_t next_tile_count_x = ceil_div(screen_width, kTileSize);
            const uint32_t next_tile_count_y = ceil_div(screen_height, kTileSize);
            const uint32_t next_tile_count = next_tile_count_x * next_tile_count_y;
            const uint32_t next_entry_capacity = compute_entry_capacity(max_splat_count);

            if (max_splat_count == max_splat_count_ &&
                next_tile_count_x == tile_count_x_ &&
                next_tile_count_y == tile_count_y_ &&
                next_entry_capacity == entry_capacity_) {
                return;
            }

            destroy();

            max_splat_count_ = max_splat_count;
            tile_count_x_ = next_tile_count_x;
            tile_count_y_ = next_tile_count_y;
            tile_count_ = next_tile_count;
            entry_capacity_ = next_entry_capacity;

            if (max_splat_count_ == 0 || tile_count_ == 0 || entry_capacity_ == 0) {
                return;
            }

            const uint64_t tile_buffer_size = static_cast<uint64_t>(tile_count_) * sizeof(uint32_t);
            const uint64_t entry_buffer_size = static_cast<uint64_t>(entry_capacity_) * sizeof(uint32_t);
            const uint64_t entry_stats_buffer_size = 4u * sizeof(uint32_t);

            const rhi::BufferDesc tile_buffer_desc{
                .size = tile_buffer_size,
                .usage = rhi::BufferUsage::StorageBuffer | rhi::BufferUsage::TransferDst,
                .memory = rhi::MemoryUsage::GpuOnly,
            };
            tile_offsets_buffer_ = rm_->create_buffer(tile_buffer_desc, "GS Tile Offsets SSBO");
            tile_counts_buffer_ = rm_->create_buffer(tile_buffer_desc, "GS Tile Counts SSBO");

            const rhi::BufferDesc entry_buffer_desc{
                .size = entry_buffer_size,
                .usage = rhi::BufferUsage::StorageBuffer,
                .memory = rhi::MemoryUsage::GpuOnly,
            };
            entry_depth_keys_buffer_ = rm_->create_buffer(entry_buffer_desc, "GS Entry Depth Keys SSBO");
            entry_tile_ids_buffer_ = rm_->create_buffer(entry_buffer_desc, "GS Entry Tile IDs SSBO");
            entry_splat_ids_buffer_ = rm_->create_buffer(entry_buffer_desc, "GS Entry Splat IDs SSBO");
            entry_indices_buffer_ = rm_->create_buffer(entry_buffer_desc, "GS Entry Indices SSBO");

            entry_stats_buffer_ = rm_->create_buffer({
                .size = entry_stats_buffer_size,
                .usage = rhi::BufferUsage::StorageBuffer | rhi::BufferUsage::TransferDst,
                .memory = rhi::MemoryUsage::GpuOnly,
            }, "GS Entry Stats SSBO");

            spdlog::info("GsTileBuffers: allocated {} tiles and {} tile-entry slots",
                         tile_count_, entry_capacity_);
        }

        /**
         * @brief Destroys all owned tile-entry buffers and resets capacity.
         */
        void destroy() {
            if (rm_ == nullptr) {
                reset_capacity();
                return;
            }

            destroy_buffer(tile_offsets_buffer_);
            destroy_buffer(tile_counts_buffer_);
            destroy_buffer(entry_depth_keys_buffer_);
            destroy_buffer(entry_tile_ids_buffer_);
            destroy_buffer(entry_splat_ids_buffer_);
            destroy_buffer(entry_indices_buffer_);
            destroy_buffer(entry_stats_buffer_);
            reset_capacity();
        }

        /** @brief Buffer containing per-tile entry offsets. */
        [[nodiscard]] rhi::BufferHandle tile_offsets_buffer() const {
            return tile_offsets_buffer_;
        }

        /** @brief Buffer containing per-tile entry counts. */
        [[nodiscard]] rhi::BufferHandle tile_counts_buffer() const {
            return tile_counts_buffer_;
        }

        /** @brief Buffer containing generated entry depth sort keys. */
        [[nodiscard]] rhi::BufferHandle entry_depth_keys_buffer() const {
            return entry_depth_keys_buffer_;
        }

        /** @brief Buffer containing generated entry tile IDs. */
        [[nodiscard]] rhi::BufferHandle entry_tile_ids_buffer() const {
            return entry_tile_ids_buffer_;
        }

        /** @brief Buffer containing generated compact visible splat IDs. */
        [[nodiscard]] rhi::BufferHandle entry_splat_ids_buffer() const {
            return entry_splat_ids_buffer_;
        }

        /** @brief Buffer containing generated entry identity indices. */
        [[nodiscard]] rhi::BufferHandle entry_indices_buffer() const {
            return entry_indices_buffer_;
        }

        /** @brief Buffer containing entry generation counters: requested, written, dropped, invalid. */
        [[nodiscard]] rhi::BufferHandle entry_stats_buffer() const {
            return entry_stats_buffer_;
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

        /** @brief Maximum number of visible splats represented by this allocation. */
        [[nodiscard]] uint32_t max_splat_count() const {
            return max_splat_count_;
        }

        /** @brief Capacity of entry buffers in uint32 entries. */
        [[nodiscard]] uint32_t entry_capacity() const {
            return entry_capacity_;
        }

    private:
        /**
         * @brief Integer ceil division helper.
         */
        static constexpr uint32_t ceil_div(const uint32_t value, const uint32_t divisor) {
            return divisor == 0 ? 0 : (value + divisor - 1u) / divisor;
        }

        /**
         * @brief Computes bounded tile-entry capacity for a maximum splat count.
         */
        static constexpr uint32_t compute_entry_capacity(const uint32_t max_splat_count) {
            const uint64_t requested_capacity =
                static_cast<uint64_t>(max_splat_count) * static_cast<uint64_t>(kAvgTilesBudget);
            const uint64_t bounded_capacity = std::min<uint64_t>(requested_capacity, kMaxSortableEntries);
            return static_cast<uint32_t>(std::min<uint64_t>(bounded_capacity, std::numeric_limits<uint32_t>::max()));
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
            entry_capacity_ = 0;
        }

        /** @brief Resource manager used to create and destroy owned buffers. */
        rhi::ResourceManager *rm_ = nullptr;

        /** @brief Buffer containing per-tile offsets into the sorted entry list. */
        rhi::BufferHandle tile_offsets_buffer_;

        /** @brief Buffer containing per-tile sorted entry counts. */
        rhi::BufferHandle tile_counts_buffer_;

        /** @brief Buffer containing generated depth keys, one per written entry. */
        rhi::BufferHandle entry_depth_keys_buffer_;

        /** @brief Buffer containing generated tile IDs, one per written entry. */
        rhi::BufferHandle entry_tile_ids_buffer_;

        /** @brief Buffer containing compact visible splat IDs, one per written entry. */
        rhi::BufferHandle entry_splat_ids_buffer_;

        /** @brief Buffer containing identity entry indices, one per written entry. */
        rhi::BufferHandle entry_indices_buffer_;

        /** @brief Buffer containing entry generation counters. */
        rhi::BufferHandle entry_stats_buffer_;

        /** @brief Maximum visible splat capacity. */
        uint32_t max_splat_count_ = 0;

        /** @brief Number of tiles along X. */
        uint32_t tile_count_x_ = 0;

        /** @brief Number of tiles along Y. */
        uint32_t tile_count_y_ = 0;

        /** @brief Total tile count. */
        uint32_t tile_count_ = 0;

        /** @brief Capacity of all entry buffers in uint32 entries. */
        uint32_t entry_capacity_ = 0;
    };
} // namespace himalaya::passes
