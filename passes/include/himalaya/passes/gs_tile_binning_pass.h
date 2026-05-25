#pragma once

/**
 * @file gs_tile_binning_pass.h
 * @brief Gaussian Splatting per-tile binning compute pass.
 */

#include <himalaya/passes/gs_tile_buffers.h>
#include <himalaya/rhi/context.h>
#include <himalaya/rhi/pipeline.h>
#include <himalaya/rhi/types.h>

#include <vulkan/vulkan.h>

#include <array>
#include <cstdint>

namespace himalaya::rhi {
    class CommandBuffer;
    class Context;
    class DescriptorManager;
    class ResourceManager;
    class ShaderCompiler;
}

namespace himalaya::framework {
    struct FrameContext;
}

namespace himalaya::passes {
    /**
     * @brief Builds compact per-tile GS entry ranges.
     *
     * The pass records count, tile-offset build, and scatter stages into an
     * existing command buffer. It replaces the earlier two global RadixSort path
     * with deterministic per-tile ranges; local ordering remains a follow-up
     * optimization after this correctness baseline.
     */
    class GsTileBinningPass {
    public:
        /** @brief Workgroup size used by tile helpers. */
        static constexpr uint32_t kWorkgroupSize = 256;

        /** @brief Push constant layout shared with gs_tile_count.comp and gs_tile_scatter.comp. */
        struct TileCoveragePushConstants {
            uint32_t max_splat_count; ///< Maximum visible splat capacity.
            uint32_t tile_count_x;    ///< Number of tiles along X.
            uint32_t tile_count_y;    ///< Number of tiles along Y.
            uint32_t _padding;        ///< Explicit 16-byte alignment padding.
        };

        /** @brief Push constant layout shared with gs_tile_offset.comp. */
        struct TileOffsetPushConstants {
            uint32_t mode;                 ///< Scan mode selected by orchestration.
            uint32_t tile_count;           ///< Total number of tiles.
            uint32_t max_entries_per_tile; ///< Retained per-tile capacity.
            uint32_t chunk_count;          ///< Number of 256-tile scan chunks.
            uint32_t entry_capacity;       ///< Total retained entry storage capacity.
            uint32_t _padding0;            ///< Explicit 16-byte alignment padding.
            uint32_t _padding1;            ///< Explicit 16-byte alignment padding.
            uint32_t _padding2;            ///< Explicit 16-byte alignment padding.
        };

        /** @brief One-time initialization: create descriptor layouts and pipelines. */
        void setup(rhi::Context &ctx,
                   rhi::ResourceManager &rm,
                   rhi::DescriptorManager &dm,
                   rhi::ShaderCompiler &sc);

        /**
         * @brief Ensures per-tile buffers match the current scene and viewport.
         */
        void ensure_capacity(uint32_t max_splat_count,
                             uint32_t screen_width,
                             uint32_t screen_height);

        /**
         * @brief Records per-tile binning commands.
         *
         * @param cmd                      Command buffer to record into.
         * @param frame_ctx                Per-frame context for global descriptor sets.
         * @param projected_splat_buffer   Compact projected splat data from projection.
         * @param depth_key_buffer         Unsorted view-space depth keys from projection.
         * @param visible_counter_buffer   Atomic visible splat counter from projection.
         * @param indirect_dispatch_buffer Unused legacy indirect buffer kept for API stability.
         * @param max_splat_count          Maximum visible splat capacity.
         * @param screen_width             Current render target width in pixels.
         * @param screen_height            Current render target height in pixels.
         */
        void record(const rhi::CommandBuffer &cmd,
                    const framework::FrameContext &frame_ctx,
                    rhi::BufferHandle projected_splat_buffer,
                    rhi::BufferHandle depth_key_buffer,
                    rhi::BufferHandle visible_counter_buffer,
                    rhi::BufferHandle indirect_dispatch_buffer,
                    uint32_t max_splat_count,
                    uint32_t screen_width,
                    uint32_t screen_height);

        /**
         * @brief Rebuilds screen-size-dependent tile buffers after swapchain resize.
         */
        void on_resize(uint32_t screen_width, uint32_t screen_height);

        /** @brief Rebuilds compute pipelines from disk shaders. */
        void rebuild_pipelines();

        /** @brief Resets scene-sized buffers and delayed runtime diagnostics. */
        void reset_scene_state();

        /** @brief Destroys pipelines, descriptor layouts, and owned buffers. */
        void destroy();

        /** @brief Returns true when all compute pipelines are available for recording. */
        [[nodiscard]] bool is_ready() const;

        /** @brief Tile-entry buffer storage owned by this pass. */
        [[nodiscard]] const GsTileBuffers &tile_buffers() const;

        /** @brief Returns true after at least one delayed stats readback completed. */
        [[nodiscard]] bool has_runtime_stats() const;

        /** @brief Latest delayed runtime statistics read back from the GPU. */
        [[nodiscard]] const GsRuntimeStats &runtime_stats() const;

    private:
        /** @brief Creates Set 3 push descriptor layouts. */
        void create_descriptor_layouts();

        /** @brief Creates or recreates compute pipelines. */
        void create_pipelines();

        /** @brief Destroys compute pipelines. */
        void destroy_pipelines();

        /** @brief Inserts barriers after transfer clears and stats visible-count copy. */
        void barrier_transfer_outputs_to_compute(const rhi::CommandBuffer &cmd) const;

        /** @brief Inserts barriers after per-tile count. */
        void barrier_count_outputs_to_compute(const rhi::CommandBuffer &cmd) const;

        /** @brief Inserts barriers after one tile offset scan stage. */
        void barrier_offset_outputs_to_compute(const rhi::CommandBuffer &cmd) const;

        /** @brief Inserts barriers after scatter before tile rendering/readback. */
        void barrier_scatter_outputs_to_compute_read(const rhi::CommandBuffer &cmd) const;

        /** @brief Reads the stats buffer copied into this frame's readback buffer on an older frame. */
        void consume_delayed_stats(uint32_t frame_index);

        /** @brief Records async GPU-to-CPU stats readback for the current frame. */
        void record_stats_readback(const rhi::CommandBuffer &cmd,
                                   uint32_t frame_index);

        /** @brief Destroys all per-frame stats readback buffers. */
        void destroy_readback_buffers();

        /** @brief Logs warning-level diagnostics for controllable degradation counters. */
        void log_runtime_stats_warnings();

        /** @brief Vulkan context. */
        rhi::Context *ctx_ = nullptr;

        /** @brief Resource manager. */
        rhi::ResourceManager *rm_ = nullptr;

        /** @brief Descriptor manager. */
        rhi::DescriptorManager *dm_ = nullptr;

        /** @brief Shader compiler. */
        rhi::ShaderCompiler *sc_ = nullptr;

        /** @brief Push descriptor layout for gs_tile_count.comp. */
        VkDescriptorSetLayout count_set3_layout_ = VK_NULL_HANDLE;

        /** @brief Push descriptor layout for gs_tile_offset.comp. */
        VkDescriptorSetLayout offset_set3_layout_ = VK_NULL_HANDLE;

        /** @brief Push descriptor layout for gs_tile_scatter.comp. */
        VkDescriptorSetLayout scatter_set3_layout_ = VK_NULL_HANDLE;

        /** @brief Compute pipeline for gs_tile_count.comp. */
        rhi::Pipeline count_pipeline_{};

        /** @brief Compute pipeline for gs_tile_offset.comp. */
        rhi::Pipeline offset_pipeline_{};

        /** @brief Compute pipeline for gs_tile_scatter.comp. */
        rhi::Pipeline scatter_pipeline_{};

        /** @brief Owned tile-entry buffers. */
        GsTileBuffers tile_buffers_;

        /** @brief Per-frame GPU-to-CPU readback buffers for delayed stats reads. */
        std::array<rhi::BufferHandle, rhi::kMaxFramesInFlight> stats_readback_buffers_{};

        /** @brief Whether each stats readback buffer has received at least one GPU copy. */
        std::array<bool, rhi::kMaxFramesInFlight> stats_readback_valid_{};

        /** @brief Latest delayed runtime statistics visible to CPU/UI/logging. */
        GsRuntimeStats runtime_stats_{};

        /** @brief True after runtime_stats_ has been populated from a delayed readback. */
        bool has_runtime_stats_ = false;

        /** @brief One-shot warning guard for dropped entry diagnostics. */
        bool warned_entry_dropped_ = false;

        /** @brief One-shot warning guard for invalid entry diagnostics. */
        bool warned_invalid_entries_ = false;
    };
} // namespace himalaya::passes
