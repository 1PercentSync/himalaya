#pragma once

/**
 * @file gs_tile_binning_pass.h
 * @brief Gaussian Splatting tile-entry compute pass.
 */

#include <himalaya/framework/radix_sort.h>
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
     * @brief Generates bounded per-tile entries from projected visible splats.
     *
     * The pass records tile-entry generation, depth sort, tile-id gather, tile-id
     * stable sort, and tile range build into an existing command buffer.
     */
    class GsTileBinningPass {
    public:
        /** @brief Workgroup size used by tile-entry generation and range helpers. */
        static constexpr uint32_t kWorkgroupSize = 256;

        /**
         * @brief Push constant layout shared with gs_tile_entry.comp.
         */
        struct EntryPushConstants {
            uint32_t max_splat_count; ///< Maximum visible splat capacity.
            uint32_t entry_capacity;  ///< Maximum number of tile entries that may be written.
            uint32_t tile_count_x;    ///< Number of tiles along X.
            uint32_t tile_count_y;    ///< Number of tiles along Y.
        };

        /**
         * @brief Push constant layout shared with gs_tile_sort_gather.comp.
         */
        struct GatherPushConstants {
            uint32_t max_entry_count; ///< Maximum tile-entry capacity.
        };

        /**
         * @brief Push constant layout shared with gs_tile_range.comp.
         */
        struct RangePushConstants {
            uint32_t max_entry_count; ///< Maximum tile-entry capacity.
            uint32_t tile_count;      ///< Total number of tiles in the render target.
        };

        /** @brief One-time initialization: create descriptor layouts, pipelines, and sorters. */
        void setup(rhi::Context &ctx,
                   rhi::ResourceManager &rm,
                   rhi::DescriptorManager &dm,
                   rhi::ShaderCompiler &sc);

        /**
         * @brief Ensures tile-entry buffers match the current scene and viewport.
         */
        void ensure_capacity(uint32_t max_splat_count,
                             uint32_t screen_width,
                             uint32_t screen_height);

        /**
         * @brief Records tile-entry generation commands.
         *
         * @param cmd                      Command buffer to record into.
         * @param frame_ctx                Per-frame context for global descriptor sets.
         * @param projected_splat_buffer   Compact projected splat data from projection.
         * @param depth_key_buffer         Unsorted depth keys from projection.
         * @param visible_counter_buffer   Atomic visible splat counter from projection.
         * @param indirect_dispatch_buffer Indirect dispatch buffer clamped to visible splat capacity.
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

        /** @brief Rebuilds compute pipelines from disk shaders. */
        void rebuild_pipelines();

        /** @brief Destroys pipelines, descriptor layouts, sorters, and owned buffers. */
        void destroy();

        /** @brief Tile-entry buffer storage owned by this pass. */
        [[nodiscard]] const GsTileBuffers &tile_buffers() const;

        /** @brief Final sorted tile IDs after tile-id stable sort. */
        [[nodiscard]] rhi::BufferHandle sorted_tile_ids_buffer() const;

        /** @brief Final sorted entry indices after tile-id stable sort. */
        [[nodiscard]] rhi::BufferHandle sorted_entry_indices_buffer() const;

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

        /** @brief Inserts a whole-buffer memory barrier. */
        void buffer_barrier(const rhi::CommandBuffer &cmd,
                            rhi::BufferHandle buffer,
                            VkPipelineStageFlags2 src_stage,
                            VkAccessFlags2 src_access,
                            VkPipelineStageFlags2 dst_stage,
                            VkAccessFlags2 dst_access) const;

        /** @brief Inserts barriers for tile-entry outputs consumed by later compute stages. */
        void barrier_entry_outputs_to_compute_read(const rhi::CommandBuffer &cmd) const;

        /** @brief Inserts barriers for gather outputs consumed by tile-id sort. */
        void barrier_gather_outputs_to_compute_read(const rhi::CommandBuffer &cmd) const;

        /** @brief Inserts barriers for tile range outputs consumed by tile rendering. */
        void barrier_range_outputs_to_compute_read(const rhi::CommandBuffer &cmd) const;

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

        /** @brief Push descriptor layout for gs_tile_entry.comp. */
        VkDescriptorSetLayout entry_set3_layout_ = VK_NULL_HANDLE;

        /** @brief Push descriptor layout for gs_tile_sort_gather.comp. */
        VkDescriptorSetLayout gather_set3_layout_ = VK_NULL_HANDLE;

        /** @brief Push descriptor layout for gs_tile_range.comp. */
        VkDescriptorSetLayout range_set3_layout_ = VK_NULL_HANDLE;

        /** @brief Compute pipeline for gs_tile_entry.comp. */
        rhi::Pipeline entry_pipeline_{};

        /** @brief Compute pipeline for gs_tile_sort_gather.comp. */
        rhi::Pipeline gather_pipeline_{};

        /** @brief Compute pipeline for gs_tile_range.comp. */
        rhi::Pipeline range_pipeline_{};

        /** @brief Stable radix sort for entry depth keys. */
        framework::RadixSort depth_sorter_;

        /** @brief Stable radix sort for tile IDs after depth-key ordering. */
        framework::RadixSort tile_sorter_;

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

        /** @brief One-shot warning guard for sort clamp diagnostics. */
        bool warned_sort_clamped_ = false;
    };
} // namespace himalaya::passes
