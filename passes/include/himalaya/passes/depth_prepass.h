#pragma once

/**
 * @file depth_prepass.h
 * @brief DepthPrePass: depth + normal pre-rendering pass (Layer 2).
 *
 * Renders visible opaque and alpha-masked mesh instances to fill the
 * depth and normal buffers before the forward pass. Enables zero-overdraw
 * in ForwardPass via EQUAL depth test.
 *
 * Two pipelines: Opaque (no discard, Early-Z guaranteed) and Mask
 * (alpha test + discard). Drawn in order: Opaque first, then Mask.
 *
 * When MSAA is active, resolves depth (MAX_BIT), normal (AVERAGE),
 * and roughness (AVERAGE) via Dynamic Rendering to 1x resolved targets.
 */

#include <himalaya/rhi/pipeline.h>

namespace himalaya::rhi {
    class Context;
    class ResourceManager;
    class DescriptorManager;
    class ShaderCompiler;
} // namespace himalaya::rhi

namespace himalaya::framework {
    class RenderGraph;
    struct FrameContext;
} // namespace himalaya::framework

namespace himalaya::passes {
    /**
     * @brief Depth + Normal PrePass — fills depth and world-space normal buffers.
     *
     * MSAA-aware: setup() and on_sample_count_changed() accept sample_count
     * to bake into both pipeline's rasterizationSamples.
     */
    class DepthPrePass {
    public:
        /**
         * @brief One-time initialization: compile shaders, create pipelines, store service pointers.
         *
         * @param ctx          Vulkan context.
         * @param rm           Resource manager for buffer/image access.
         * @param dm           Descriptor manager for set binding.
         * @param sc           Shader compiler for GLSL -> SPIR-V.
         * @param sample_count MSAA sample count (1 = no MSAA).
         */
        void setup(rhi::Context &ctx,
                   rhi::ResourceManager &rm,
                   rhi::DescriptorManager &dm,
                   rhi::ShaderCompiler &sc,
                   uint32_t sample_count);

        /**
         * @brief Rebuild pipelines when MSAA sample count changes.
         *
         * Caller must guarantee GPU is idle before calling (vkQueueWaitIdle).
         *
         * @param sample_count New MSAA sample count.
         */
        void on_sample_count_changed(uint32_t sample_count);

        /**
         * @brief Register RG resource usage and provide the execute callback.
         *
         * @param rg  Render graph to add the pass to.
         * @param ctx Per-frame context with RG resource IDs and scene data.
         */
        void record(framework::RenderGraph &rg, const framework::FrameContext &ctx) const;

        /**
         * @brief Rebuild pipelines by recompiling shaders from disk.
         *
         * Caller must guarantee GPU is idle before calling (vkQueueWaitIdle).
         * Uses the current sample count stored from the last setup() or
         * on_sample_count_changed() call.
         */
        void rebuild_pipelines();

        /**
         * @brief Destroy pipelines and release owned resources.
         */
        void destroy() const;

    private:
        /**
         * @brief Create (or recreate) both Opaque and Mask graphics pipelines.
         *
         * Shared by setup() and on_sample_count_changed(). Destroys
         * previous pipelines if they exist before creating new ones.
         *
         * @param sample_count MSAA sample count to bake into pipelines.
         */
        void create_pipelines(uint32_t sample_count);

        // ---- Service pointers (set in setup) ----

        /** @brief Vulkan context. */
        rhi::Context *ctx_ = nullptr;

        /** @brief Resource manager for buffer/image access during recording. */
        rhi::ResourceManager *rm_ = nullptr;

        /** @brief Descriptor manager for set binding during recording. */
        rhi::DescriptorManager *dm_ = nullptr;

        /** @brief Shader compiler for GLSL -> SPIR-V compilation. */
        rhi::ShaderCompiler *sc_ = nullptr;

        // ---- Configuration ----

        /** @brief Current MSAA sample count (for rebuild_pipelines). */
        uint32_t current_sample_count_ = 0;

        // ---- Owned resources ----

        /** @brief Opaque geometry pipeline (no discard, Early-Z guaranteed). */
        rhi::Pipeline opaque_pipeline_;

        /** @brief Alpha Mask geometry pipeline (alpha test + discard). */
        rhi::Pipeline mask_pipeline_;
    };
} // namespace himalaya::passes
