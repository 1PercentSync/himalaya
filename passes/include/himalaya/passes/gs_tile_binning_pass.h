#pragma once

/**
 * @file gs_tile_binning_pass.h
 * @brief Gaussian Splatting tile-binning compute pass.
 */

#include <himalaya/passes/gs_tile_buffers.h>
#include <himalaya/rhi/pipeline.h>
#include <himalaya/rhi/types.h>

#include <vulkan/vulkan.h>

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
     * @brief Builds per-tile splat lists from sorted projected splats.
     *
     * The pass records three compute stages into an existing command buffer:
     * tile coverage counting, tile-count prefix scan, and tile-list scatter.
     * It owns all tile-binning output buffers and exposes them for the future
     * tile-rendering pass.
     */
    class GsTileBinningPass {
    public:
        /** @brief Workgroup size used by all tile-binning shaders. */
        static constexpr uint32_t kWorkgroupSize = 256;

        /**
         * @brief Push constant layout shared with gs_tile_count.comp.
         */
        struct CountPushConstants {
            uint32_t max_splat_count; ///< Maximum visible splat capacity.
            uint32_t tile_count_x;    ///< Number of tiles along X.
            uint32_t tile_count_y;    ///< Number of tiles along Y.
            uint32_t _padding;        ///< Explicit padding for 16-byte alignment.
        };

        /**
         * @brief Push constant layout shared with gs_tile_scan.comp.
         */
        struct ScanPushConstants {
            uint32_t mode;        ///< Scan mode selected by record().
            uint32_t tile_count;  ///< Total number of tiles.
            uint32_t chunk_count; ///< Number of 256-tile chunks.
            uint32_t _padding;    ///< Explicit padding for 16-byte alignment.
        };

        /**
         * @brief Push constant layout shared with gs_tile_scatter.comp.
         */
        struct ScatterPushConstants {
            uint32_t max_splat_count;        ///< Maximum visible splat capacity.
            uint32_t tile_count_x;           ///< Number of tiles along X.
            uint32_t tile_count_y;           ///< Number of tiles along Y.
            uint32_t tile_splat_id_capacity; ///< Capacity of tile_splat_ids[] in uint entries.
        };

        /** @brief One-time initialization: create descriptor layouts and pipelines. */
        void setup(rhi::Context &ctx,
                   rhi::ResourceManager &rm,
                   rhi::DescriptorManager &dm,
                   rhi::ShaderCompiler &sc);

        /**
         * @brief Ensures tile-binning buffers match the current scene and viewport.
         */
        void ensure_capacity(uint32_t max_splat_count,
                             uint32_t screen_width,
                             uint32_t screen_height);

        /**
         * @brief Records count, scan, and scatter commands.
         *
         * @param cmd                      Command buffer to record into.
         * @param frame_ctx                Per-frame context for global descriptor sets.
         * @param projected_splat_buffer   Compact projected splat data from projection.
         * @param sorted_splat_index_buffer Sorted value buffer from radix sort.
         * @param visible_counter_buffer   Atomic visible splat counter from projection.
         * @param indirect_dispatch_buffer Indirect dispatch buffer prepared by radix sort.
         * @param max_splat_count          Maximum visible splat capacity.
         * @param screen_width             Current render target width in pixels.
         * @param screen_height            Current render target height in pixels.
         */
        void record(const rhi::CommandBuffer &cmd,
                    const framework::FrameContext &frame_ctx,
                    rhi::BufferHandle projected_splat_buffer,
                    rhi::BufferHandle sorted_splat_index_buffer,
                    rhi::BufferHandle visible_counter_buffer,
                    rhi::BufferHandle indirect_dispatch_buffer,
                    uint32_t max_splat_count,
                    uint32_t screen_width,
                    uint32_t screen_height);

        /** @brief Rebuilds compute pipelines from disk shaders. */
        void rebuild_pipelines();

        /** @brief Destroys pipelines, descriptor layouts, and owned buffers. */
        void destroy();

        /** @brief Tile-binning buffer storage owned by this pass. */
        [[nodiscard]] const GsTileBuffers &tile_buffers() const;

    private:
        /** @brief Tile scan execution modes. */
        enum class ScanMode : uint32_t {
            ScanTileChunks = 0,
            ScanChunkSums = 1,
            AddChunkOffsets = 2,
        };

        /** @brief Creates Set 3 push descriptor layouts. */
        void create_descriptor_layouts();

        /** @brief Creates or recreates compute pipelines. */
        void create_pipelines();

        /** @brief Destroys all compute pipelines. */
        void destroy_pipelines();

        /** @brief Inserts a whole-buffer memory barrier. */
        void buffer_barrier(const rhi::CommandBuffer &cmd,
                            rhi::BufferHandle buffer,
                            VkPipelineStageFlags2 src_stage,
                            VkAccessFlags2 src_access,
                            VkPipelineStageFlags2 dst_stage,
                            VkAccessFlags2 dst_access) const;

        /** @brief Inserts barriers for all tile buffers that may be read by later stages. */
        void barrier_tile_outputs_to_compute_read(const rhi::CommandBuffer &cmd) const;

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

        /** @brief Push descriptor layout for gs_tile_scan.comp. */
        VkDescriptorSetLayout scan_set3_layout_ = VK_NULL_HANDLE;

        /** @brief Push descriptor layout for gs_tile_scatter.comp. */
        VkDescriptorSetLayout scatter_set3_layout_ = VK_NULL_HANDLE;

        /** @brief Compute pipeline for gs_tile_count.comp. */
        rhi::Pipeline count_pipeline_{};

        /** @brief Compute pipeline for gs_tile_scan.comp. */
        rhi::Pipeline scan_pipeline_{};

        /** @brief Compute pipeline for gs_tile_scatter.comp. */
        rhi::Pipeline scatter_pipeline_{};

        /** @brief Owned tile-binning buffers. */
        GsTileBuffers tile_buffers_;
    };
} // namespace himalaya::passes
