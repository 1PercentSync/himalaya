#pragma once

/**
 * @file gs_tile_buffers.h
 * @brief Gaussian Splatting per-tile binning buffer storage.
 */

#include <himalaya/rhi/resources.h>
#include <himalaya/rhi/types.h>

#include <algorithm>
#include <cstdint>
#include <limits>

#include <spdlog/spdlog.h>

namespace himalaya::passes {
    /**
     * @brief Runtime statistics produced by the GS per-tile pipeline.
     */
    struct GsRuntimeStats {
        uint32_t visible_splats = 0;       ///< Visible splats reported by projection.
        uint32_t entry_requested = 0;      ///< Total tile entries requested before capacity clipping.
        uint32_t entry_written = 0;        ///< Tile entries successfully written.
        uint32_t entry_dropped = 0;        ///< Tile entries dropped due to capacity limits.
        uint32_t invalid_entries = 0;      ///< Invalid splat or tile entries detected by shaders.
        uint32_t max_tile_requested = 0;   ///< Maximum requested entry count observed for one tile.
    };

    static_assert(sizeof(GsRuntimeStats) == 24, "GsRuntimeStats must match shader std430 uint layout");

    /**
     * @brief Owns GPU buffers produced and consumed by the GS per-tile pipeline.
     *
     * The pass builds compact per-tile entry ranges and writes splat IDs into
     * those ranges. It intentionally avoids the earlier two global RadixSort
     * passes while keeping overflow deterministic and visible in diagnostics.
     */
    class GsTileBuffers {
    public:
        /** @brief Tile width and height in pixels. Must match GS shaders. */
        static constexpr uint32_t kTileSize = 16;

        /** @brief Average tiles-per-splat budget used for total entry storage. */
        static constexpr uint32_t kAvgTilesBudget = 16;

        /** @brief Absolute retained entry storage limit for the per-tile pipeline. */
        static constexpr uint32_t kMaxTotalEntries = 64u * 1024u * 1024u;

        /** @brief Maximum entries retained for one tile before deterministic overflow. */
        static constexpr uint32_t kMaxEntriesPerTile = 1024u * 1024u;

        /** @brief Workgroup size used by tile scan helpers. */
        static constexpr uint32_t kScanWorkgroupSize = 256;

        /**
         * @brief Stores the resource manager used for all buffer operations.
         */
        void setup(rhi::ResourceManager &rm) {
            rm_ = &rm;
        }

        /**
         * @brief Ensures all per-tile buffers match the requested viewport.
         *
         * Passing zero dimensions or zero splat capacity destroys existing
         * buffers. Total entry storage is derived from the scene splat count
         * and capped by kMaxTotalEntries; per-tile retention is capped by
         * kMaxEntriesPerTile during offset build.
         */
        void ensure_capacity(const uint32_t max_splat_count,
                             const uint32_t screen_width,
                             const uint32_t screen_height) {
            const uint32_t next_tile_count_x = ceil_div(screen_width, kTileSize);
            const uint32_t next_tile_count_y = ceil_div(screen_height, kTileSize);
            const uint32_t next_tile_count = next_tile_count_x * next_tile_count_y;
            const uint32_t next_scan_chunk_count = ceil_div(next_tile_count, kScanWorkgroupSize);
            const uint32_t next_entry_capacity = compute_entry_capacity(max_splat_count);

            if (max_splat_count == max_splat_count_ &&
                next_tile_count_x == tile_count_x_ &&
                next_tile_count_y == tile_count_y_ &&
                next_tile_count == tile_count_ &&
                next_scan_chunk_count == scan_chunk_count_ &&
                next_entry_capacity == entry_capacity_) {
                return;
            }

            destroy();

            max_splat_count_ = max_splat_count;
            tile_count_x_ = next_tile_count_x;
            tile_count_y_ = next_tile_count_y;
            tile_count_ = next_tile_count;
            scan_chunk_count_ = next_scan_chunk_count;
            entry_capacity_ = next_entry_capacity;

            if (max_splat_count_ == 0 || tile_count_ == 0 || entry_capacity_ == 0 || scan_chunk_count_ == 0) {
                return;
            }

            const uint64_t tile_buffer_size = static_cast<uint64_t>(tile_count_) * sizeof(uint32_t);
            const uint64_t chunk_buffer_size = static_cast<uint64_t>(scan_chunk_count_) * sizeof(uint32_t);
            const uint64_t entry_buffer_size = static_cast<uint64_t>(entry_capacity_) * sizeof(uint32_t);
            const uint64_t entry_stats_buffer_size = sizeof(GsRuntimeStats);

            const rhi::BufferDesc tile_buffer_desc{
                .size = tile_buffer_size,
                .usage = rhi::BufferUsage::StorageBuffer | rhi::BufferUsage::TransferDst,
                .memory = rhi::MemoryUsage::GpuOnly,
            };
            tile_requested_counts_buffer_ = rm_->create_buffer(tile_buffer_desc, "GS Tile Requested Counts SSBO");
            tile_offsets_buffer_ = rm_->create_buffer(tile_buffer_desc, "GS Tile Offsets SSBO");
            tile_counts_buffer_ = rm_->create_buffer(tile_buffer_desc, "GS Tile Counts SSBO");
            tile_cursors_buffer_ = rm_->create_buffer(tile_buffer_desc, "GS Tile Cursors SSBO");

            const rhi::BufferDesc chunk_buffer_desc{
                .size = chunk_buffer_size,
                .usage = rhi::BufferUsage::StorageBuffer | rhi::BufferUsage::TransferDst,
                .memory = rhi::MemoryUsage::GpuOnly,
            };
            scan_chunk_sums_buffer_ = rm_->create_buffer(chunk_buffer_desc, "GS Tile Scan Chunk Sums SSBO");
            scan_chunk_offsets_buffer_ = rm_->create_buffer(chunk_buffer_desc, "GS Tile Scan Chunk Offsets SSBO");

            const rhi::BufferDesc entry_buffer_desc{
                .size = entry_buffer_size,
                .usage = rhi::BufferUsage::StorageBuffer,
                .memory = rhi::MemoryUsage::GpuOnly,
            };
            entry_depth_keys_buffer_ = rm_->create_buffer(entry_buffer_desc, "GS Per-Tile Entry Depth Keys SSBO");
            entry_splat_ids_buffer_ = rm_->create_buffer(entry_buffer_desc, "GS Per-Tile Entry Splat IDs SSBO");

            entry_stats_buffer_ = rm_->create_buffer({
                .size = entry_stats_buffer_size,
                .usage = rhi::BufferUsage::StorageBuffer | rhi::BufferUsage::TransferDst | rhi::BufferUsage::TransferSrc,
                .memory = rhi::MemoryUsage::GpuOnly,
            }, "GS Entry Stats SSBO");

            spdlog::info("GsTileBuffers: allocated {} tiles, {} scan chunks, and {} total per-tile entry slots",
                         tile_count_,
                         scan_chunk_count_,
                         entry_capacity_);
        }

        /**
         * @brief Destroys all owned per-tile buffers and resets capacity.
         */
        void destroy() {
            if (rm_ == nullptr) {
                reset_capacity();
                return;
            }

            destroy_buffer(tile_requested_counts_buffer_);
            destroy_buffer(tile_offsets_buffer_);
            destroy_buffer(tile_counts_buffer_);
            destroy_buffer(tile_cursors_buffer_);
            destroy_buffer(scan_chunk_sums_buffer_);
            destroy_buffer(scan_chunk_offsets_buffer_);
            destroy_buffer(entry_depth_keys_buffer_);
            destroy_buffer(entry_splat_ids_buffer_);
            destroy_buffer(entry_stats_buffer_);
            reset_capacity();
        }

        /** @brief Buffer containing requested entry counts per tile. */
        [[nodiscard]] rhi::BufferHandle tile_requested_counts_buffer() const {
            return tile_requested_counts_buffer_;
        }

        /** @brief Buffer containing per-tile entry offsets into compact entry storage. */
        [[nodiscard]] rhi::BufferHandle tile_offsets_buffer() const {
            return tile_offsets_buffer_;
        }

        /** @brief Buffer containing retained entry counts per tile. */
        [[nodiscard]] rhi::BufferHandle tile_counts_buffer() const {
            return tile_counts_buffer_;
        }

        /** @brief Buffer containing per-tile scatter cursors. */
        [[nodiscard]] rhi::BufferHandle tile_cursors_buffer() const {
            return tile_cursors_buffer_;
        }

        /** @brief Buffer containing padded per-chunk entry sums for tile scan. */
        [[nodiscard]] rhi::BufferHandle scan_chunk_sums_buffer() const {
            return scan_chunk_sums_buffer_;
        }

        /** @brief Buffer containing exclusive per-chunk offsets for tile scan. */
        [[nodiscard]] rhi::BufferHandle scan_chunk_offsets_buffer() const {
            return scan_chunk_offsets_buffer_;
        }

        /** @brief Buffer containing per-entry view-space depth keys. */
        [[nodiscard]] rhi::BufferHandle entry_depth_keys_buffer() const {
            return entry_depth_keys_buffer_;
        }

        /** @brief Buffer containing compact visible splat IDs for each retained entry. */
        [[nodiscard]] rhi::BufferHandle entry_splat_ids_buffer() const {
            return entry_splat_ids_buffer_;
        }

        /** @brief Buffer containing runtime stats copied back to CPU with delayed readback. */
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

        /** @brief Number of 256-tile scan chunks. */
        [[nodiscard]] uint32_t scan_chunk_count() const {
            return scan_chunk_count_;
        }

        /** @brief Maximum number of visible splats represented by this allocation. */
        [[nodiscard]] uint32_t max_splat_count() const {
            return max_splat_count_;
        }

        /** @brief Maximum retained entries per tile. */
        [[nodiscard]] uint32_t max_entries_per_tile() const {
            return kMaxEntriesPerTile;
        }

        /** @brief Total entry storage capacity in uint32 entries. */
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
         * @brief Computes bounded total entry capacity for the current scene.
         */
        static constexpr uint32_t compute_entry_capacity(const uint32_t max_splat_count) {
            const uint64_t requested_capacity =
                static_cast<uint64_t>(max_splat_count) * static_cast<uint64_t>(kAvgTilesBudget);
            const uint64_t bounded_capacity = std::min<uint64_t>(requested_capacity, kMaxTotalEntries);
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
            scan_chunk_count_ = 0;
            entry_capacity_ = 0;
        }

        /** @brief Resource manager used to create and destroy owned buffers. */
        rhi::ResourceManager *rm_ = nullptr;

        /** @brief Buffer containing requested entry counts per tile. */
        rhi::BufferHandle tile_requested_counts_buffer_;

        /** @brief Buffer containing per-tile offsets into compact entry storage. */
        rhi::BufferHandle tile_offsets_buffer_;

        /** @brief Buffer containing retained entry counts per tile. */
        rhi::BufferHandle tile_counts_buffer_;

        /** @brief Buffer containing per-tile scatter cursors. */
        rhi::BufferHandle tile_cursors_buffer_;

        /** @brief Buffer containing padded per-chunk sums for tile offset scan. */
        rhi::BufferHandle scan_chunk_sums_buffer_;

        /** @brief Buffer containing exclusive per-chunk offsets. */
        rhi::BufferHandle scan_chunk_offsets_buffer_;

        /** @brief Buffer containing per-entry view-space depth keys. */
        rhi::BufferHandle entry_depth_keys_buffer_;

        /** @brief Buffer containing compact visible splat IDs for each retained entry. */
        rhi::BufferHandle entry_splat_ids_buffer_;

        /** @brief Buffer containing runtime stats copied back to CPU with delayed readback. */
        rhi::BufferHandle entry_stats_buffer_;

        /** @brief Maximum visible splat capacity. */
        uint32_t max_splat_count_ = 0;

        /** @brief Number of tiles along X. */
        uint32_t tile_count_x_ = 0;

        /** @brief Number of tiles along Y. */
        uint32_t tile_count_y_ = 0;

        /** @brief Total tile count. */
        uint32_t tile_count_ = 0;

        /** @brief Number of scan chunks. */
        uint32_t scan_chunk_count_ = 0;

        /** @brief Total entry storage capacity. */
        uint32_t entry_capacity_ = 0;
    };
} // namespace himalaya::passes
