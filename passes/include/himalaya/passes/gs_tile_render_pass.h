#pragma once

/**
 * @file gs_tile_render_pass.h
 * @brief Gaussian Splatting tile rendering compute pass.
 */

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
    class GsTileBuffers;

    /**
     * @brief Shades sorted GS tile entries into a storage image.
     *
     * One 16x16 compute workgroup maps to one tile. The pass reads projected
     * splat attributes, per-tile ranges, and final sorted entry indices, then
     * writes an RG-managed R16G16B16A16F color image for PresentPass sampling.
     */
    class GsTileRenderPass {
    public:
        /** @brief Push constant layout shared with shaders/gs/gs_tile_render.comp. */
        struct PushConstants {
            uint32_t tile_count_x; ///< Number of tiles along X.
            uint32_t tile_count_y; ///< Number of tiles along Y.
        };

        /** @brief One-time initialization: creates Set 3 layout and compute pipeline. */
        void setup(rhi::Context &ctx,
                   rhi::ResourceManager &rm,
                   rhi::DescriptorManager &dm,
                   rhi::ShaderCompiler &sc);

        /**
         * @brief Records tile rendering commands into an existing GS compute pass.
         *
         * @param cmd Command buffer to record into.
         * @param frame_ctx Per-frame context for global descriptor sets.
         * @param gs_color_image Storage image receiving the rendered GS color.
         * @param projected_splat_buffer Projected visible splat data.
         * @param tile_buffers Tile range and entry data from GsTileBinningPass.
         * @param sorted_entry_indices_buffer Final sorted entry index buffer.
         */
        void record(const rhi::CommandBuffer &cmd,
                    const framework::FrameContext &frame_ctx,
                    rhi::ImageHandle gs_color_image,
                    rhi::BufferHandle projected_splat_buffer,
                    const GsTileBuffers &tile_buffers,
                    rhi::BufferHandle sorted_entry_indices_buffer) const;

        /** @brief Rebuilds the compute pipeline from disk shaders. */
        void rebuild_pipelines();

        /** @brief Destroys pipeline and Set 3 layout. */
        void destroy();

        /** @brief Returns true when the compute pipeline is available for recording. */
        [[nodiscard]] bool is_ready() const;

    private:
        /** @brief Creates or recreates the compute pipeline. */
        void create_pipeline();

        /** @brief Vulkan context. */
        rhi::Context *ctx_ = nullptr;

        /** @brief Resource manager. */
        rhi::ResourceManager *rm_ = nullptr;

        /** @brief Descriptor manager. */
        rhi::DescriptorManager *dm_ = nullptr;

        /** @brief Shader compiler. */
        rhi::ShaderCompiler *sc_ = nullptr;

        /** @brief Set 3 push descriptor layout for GS tile render inputs and output image. */
        VkDescriptorSetLayout set3_layout_ = VK_NULL_HANDLE;

        /** @brief Compute pipeline for gs_tile_render.comp. */
        rhi::Pipeline pipeline_{};
    };
} // namespace himalaya::passes
