#pragma once

/**
 * @file gs_projection_pass.h
 * @brief Gaussian Splatting projection pass (passes layer).
 *
 * Owns the projection-stage intermediate buffers and compute pipeline.
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
    class GsGpuData;
    struct FrameContext;
}

namespace himalaya::passes {
    /**
     * @brief Projects 3D Gaussian splats into compacted 2D splat data.
     *
     * The pass reads GsGpuData core/SH buffers, writes compacted visible splat
     * data, and increments a GPU-visible atomic counter. The indirect dispatch
     * buffer is created here but filled later by the radix-sort orchestration.
     *
     * This class records commands directly into an existing command buffer so it
     * can be used inside the future single RenderGraph pass that contains all GS
     * compute stages and their manual buffer barriers.
     */
    class GsProjectionPass {
    public:
        /**
         * @brief Push constant layout shared with shaders/gs/gs_projection.comp.
         */
        struct PushConstants {
            uint32_t splat_offset;  ///< Global splat offset into merged core/output buffers.
            uint32_t splat_count;   ///< Number of splats in this SH-degree group.
            uint32_t sh_degree;     ///< Max SH degree for this dispatch group (0-3).
            uint32_t screen_width;  ///< Current render target width in pixels.
            uint32_t screen_height; ///< Current render target height in pixels.
        };

        /** @brief One-time initialization: create Set 3 layout and compute pipeline. */
        void setup(rhi::Context &ctx,
                   rhi::ResourceManager &rm,
                   rhi::DescriptorManager &dm,
                   rhi::ShaderCompiler &sc);

        /**
         * @brief Ensures projection intermediate buffers can hold max_splat_count splats.
         *
         * Recreates all owned buffers when capacity changes. Passing 0 destroys
         * existing buffers and resets capacity.
         */
        void ensure_capacity(uint32_t max_splat_count);

        /**
         * @brief Records counter clear and projection dispatches.
         *
         * @param cmd           Command buffer to record into.
         * @param frame_ctx     Per-frame context for descriptor set selection.
         * @param gs_data       Uploaded GS scene buffers and SH dispatch groups.
         * @param screen_width  Current render target width in pixels.
         * @param screen_height Current render target height in pixels.
         */
        void record(const rhi::CommandBuffer &cmd,
                    const framework::FrameContext &frame_ctx,
                    const framework::GsGpuData &gs_data,
                    uint32_t screen_width,
                    uint32_t screen_height);

        /** @brief Rebuilds the compute pipeline from disk shaders. */
        void rebuild_pipelines();

        /** @brief Destroys pipeline, Set 3 layout, and owned buffers. */
        void destroy();

        /** @brief Projected 2D visible splat data SSBO. */
        [[nodiscard]] rhi::BufferHandle projected_splat_buffer() const;

        /** @brief Unsorted depth key buffer, one uint32 per visible splat. */
        [[nodiscard]] rhi::BufferHandle depth_key_buffer() const;

        /** @brief Unsorted splat index/value buffer, one uint32 per visible splat. */
        [[nodiscard]] rhi::BufferHandle splat_index_buffer() const;

        /** @brief Atomic visible splat counter buffer. */
        [[nodiscard]] rhi::BufferHandle visible_counter_buffer() const;

        /** @brief Indirect dispatch buffer owned by projection stage and filled by Step 4. */
        [[nodiscard]] rhi::BufferHandle indirect_dispatch_buffer() const;

        /** @brief Current maximum splat capacity of owned buffers. */
        [[nodiscard]] uint32_t max_splat_count() const;

    private:
        /** @brief Creates or recreates the compute pipeline. */
        void create_pipeline();

        /** @brief Destroys only owned GPU buffers. */
        void destroy_buffers();

        /** @brief Inserts a transfer-write to compute-read/write barrier for the visible counter. */
        void barrier_counter_clear_to_compute(const rhi::CommandBuffer &cmd) const;

        // ---- Service pointers ----

        /** @brief Vulkan context. */
        rhi::Context *ctx_ = nullptr;

        /** @brief Resource manager. */
        rhi::ResourceManager *rm_ = nullptr;

        /** @brief Descriptor manager. */
        rhi::DescriptorManager *dm_ = nullptr;

        /** @brief Shader compiler. */
        rhi::ShaderCompiler *sc_ = nullptr;

        // ---- Owned Vulkan objects ----

        /** @brief Set 3 push descriptor layout for GS projection buffers. */
        VkDescriptorSetLayout set3_layout_ = VK_NULL_HANDLE;

        /** @brief Compute pipeline for gs_projection.comp. */
        rhi::Pipeline pipeline_{};

        // ---- Owned buffers ----

        /** @brief Projected 2D splat output SSBO. */
        rhi::BufferHandle projected_splat_buffer_;

        /** @brief Depth key output buffer for radix sort input. */
        rhi::BufferHandle depth_key_buffer_;

        /** @brief Splat index/value output buffer for radix sort input. */
        rhi::BufferHandle splat_index_buffer_;

        /** @brief Atomic visible splat counter. */
        rhi::BufferHandle visible_counter_buffer_;

        /** @brief VkDispatchIndirectCommand buffer, populated by Step 4. */
        rhi::BufferHandle indirect_dispatch_buffer_;

        /** @brief Maximum splat count currently allocated. */
        uint32_t max_splat_count_ = 0;
    };
} // namespace himalaya::passes
