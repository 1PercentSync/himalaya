#pragma once

/**
 * @file skybox_pass.h
 * @brief SkyboxPass: cubemap sky rendering pass (Layer 2).
 *
 * Renders the environment cubemap as a sky background using a fullscreen
 * triangle with world-space direction computation. Placed after ForwardPass
 * and before TonemappingPass in the frame pipeline.
 *
 * Non-MSAA pass: always renders at 1x into resolved hdr_color, reads
 * resolved depth with GREATER_OR_EQUAL to only draw where no geometry exists.
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
     * @brief Skybox pass — fullscreen cubemap sampling with depth rejection.
     *
     * Non-MSAA pass: no on_resize() or on_sample_count_changed().
     * Depth test GREATER_OR_EQUAL with write OFF ensures sky only fills
     * pixels at the reverse-Z far plane (depth == 0.0).
     */
    class SkyboxPass {
    public:
        /**
         * @brief One-time initialization: compile shaders, create pipeline, store service pointers.
         *
         * @param ctx Vulkan context.
         * @param rm  Resource manager.
         * @param dm  Descriptor manager.
         * @param sc  Shader compiler.
         */
        void setup(rhi::Context &ctx,
                   rhi::ResourceManager &rm,
                   rhi::DescriptorManager &dm,
                   rhi::ShaderCompiler &sc);

        /**
         * @brief Register RG resource usage and provide the execute callback.
         *
         * @param rg  Render graph to add the pass to.
         * @param ctx Per-frame context with RG resource IDs.
         */
        void record(framework::RenderGraph &rg, const framework::FrameContext &ctx) const;

        /**
         * @brief Rebuild pipeline by recompiling shaders from disk.
         *
         * Caller must guarantee GPU is idle before calling (vkQueueWaitIdle).
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
         * Shared by setup() and potential future hot-reload.
         */
        void create_pipelines();

        // ---- Service pointers ----

        /** @brief Vulkan context. */
        rhi::Context *ctx_ = nullptr;

        /** @brief Resource manager for image access during recording. */
        rhi::ResourceManager *rm_ = nullptr;

        /** @brief Descriptor manager for set binding. */
        rhi::DescriptorManager *dm_ = nullptr;

        /** @brief Shader compiler. */
        rhi::ShaderCompiler *sc_ = nullptr;

        // ---- Owned resources ----

        /** @brief Skybox fullscreen pipeline. */
        rhi::Pipeline pipeline_;
    };
} // namespace himalaya::passes
