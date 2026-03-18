#pragma once

/**
 * @file forward_pass.h
 * @brief ForwardPass: main forward lighting pass (Layer 2).
 *
 * Renders visible mesh instances into the HDR color buffer with depth testing.
 * Extracted from the Renderer's inline RG lambda to a standalone pass class.
 *
 * Depth compare EQUAL + write OFF: relies on DepthPrePass to fill the
 * depth buffer first, achieving zero-overdraw for opaque geometry.
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
     * @brief Forward lighting pass — draws visible meshes into HDR color + depth.
     *
     * MSAA-aware: setup() and on_sample_count_changed() accept sample_count
     * to bake into the pipeline's rasterizationSamples.
     */
    class ForwardPass {
    public:
        /**
         * @brief One-time initialization: compile shaders, create pipeline, store service pointers.
         *
         * @param ctx          Vulkan context.
         * @param rm           Resource manager for buffer/image access.
         * @param dm           Descriptor manager for set binding.
         * @param sc           Shader compiler for GLSL → SPIR-V.
         * @param sample_count MSAA sample count (1 = no MSAA).
         */
        void setup(rhi::Context &ctx,
                   rhi::ResourceManager &rm,
                   rhi::DescriptorManager &dm,
                   rhi::ShaderCompiler &sc,
                   uint32_t sample_count);

        /**
         * @brief Rebuild pipeline when MSAA sample count changes.
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
         * @brief Rebuild pipeline by recompiling shaders from disk.
         *
         * Caller must guarantee GPU is idle before calling (vkQueueWaitIdle).
         * Uses the current sample count stored from the last setup() or
         * on_sample_count_changed() call.
         */
        void rebuild_pipelines();

        /**
         * @brief Destroy pipeline and release owned resources.
         */
        void destroy() const;

    private:
        /**
         * @brief Create (or recreate) the graphics pipeline.
         *
         * Shared by setup() and on_sample_count_changed(). Destroys the
         * previous pipeline if one exists before creating the new one.
         *
         * @param sample_count MSAA sample count to bake into the pipeline.
         */
        void create_pipelines(uint32_t sample_count);

        // ---- Service pointers (set in setup, accessed via this in lambdas) ----

        /** @brief Vulkan context. */
        rhi::Context *ctx_ = nullptr;

        /** @brief Resource manager for buffer/image access during recording. */
        rhi::ResourceManager *rm_ = nullptr;

        /** @brief Descriptor manager for set binding during recording. */
        rhi::DescriptorManager *dm_ = nullptr;

        /** @brief Shader compiler for GLSL → SPIR-V compilation. */
        rhi::ShaderCompiler *sc_ = nullptr;

        // ---- Configuration ----

        /** @brief Current MSAA sample count (for rebuild_pipelines). */
        uint32_t current_sample_count_ = 0;

        // ---- Owned resources ----

        /** @brief Forward lighting graphics pipeline. */
        rhi::Pipeline pipeline_;
    };
} // namespace himalaya::passes
