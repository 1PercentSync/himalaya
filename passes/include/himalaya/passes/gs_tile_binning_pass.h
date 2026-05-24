#pragma once

/**
 * @file gs_tile_binning_pass.h
 * @brief Gaussian Splatting tile-entry compute pass.
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
     * @brief Generates bounded per-tile entries from projected visible splats.
     *
     * The pass records the tile-entry generation stage into an existing command
     * buffer. Later Step 5.5 items sort these entries and build per-tile ranges.
     */
    class GsTileBinningPass {
    public:
        /** @brief Workgroup size used by tile-entry generation. */
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

        /** @brief One-time initialization: create descriptor layout and pipeline. */
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

        /** @brief Rebuilds compute pipeline from disk shader. */
        void rebuild_pipelines();

        /** @brief Destroys pipeline, descriptor layout, and owned buffers. */
        void destroy();

        /** @brief Tile-entry buffer storage owned by this pass. */
        [[nodiscard]] const GsTileBuffers &tile_buffers() const;

    private:
        /** @brief Creates Set 3 push descriptor layout. */
        void create_descriptor_layouts();

        /** @brief Creates or recreates compute pipeline. */
        void create_pipelines();

        /** @brief Destroys compute pipeline. */
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

        /** @brief Compute pipeline for gs_tile_entry.comp. */
        rhi::Pipeline entry_pipeline_{};

        /** @brief Owned tile-entry buffers. */
        GsTileBuffers tile_buffers_;
    };
} // namespace himalaya::passes
