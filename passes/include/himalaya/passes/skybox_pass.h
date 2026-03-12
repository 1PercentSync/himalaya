#pragma once

/**
 * @file skybox_pass.h
 * @brief SkyboxPass: environment cubemap sky rendering (Layer 2).
 *
 * Renders the sky background using the IBL intermediate cubemap.
 * Runs after ForwardPass and before TonemappingPass, rendering to
 * resolved 1x hdr_color with resolved depth read (GREATER_OR_EQUAL).
 *
 * Lightweight pass: no on_resize() or on_sample_count_changed() —
 * no resolution/MSAA-dependent private resources, pipeline is always 1x.
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
     * @brief Skybox pass — fullscreen cubemap sampling at sky pixels.
     *
     * Uses GREATER_OR_EQUAL depth test with depth write OFF (Reverse-Z:
     * sky depth == 0.0, geometry depth > 0.0). Only sky pixels pass.
     * Early-Z rejects geometry-covered fragments before FS execution.
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
         * Resources: hdr_color (ReadWrite, ColorAttachment), depth (Read, DepthAttachment).
         *
         * @param rg  Render graph to add the pass to.
         * @param ctx Per-frame context with RG resource IDs.
         */
        void record(framework::RenderGraph &rg, const framework::FrameContext &ctx) const;

        /**
         * @brief Destroy pipeline.
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

        /** @brief Descriptor manager for set layouts and descriptor binding. */
        rhi::DescriptorManager *dm_ = nullptr;

        /** @brief Shader compiler. */
        rhi::ShaderCompiler *sc_ = nullptr;

        // ---- Owned resources ----

        /** @brief Skybox fullscreen pipeline. */
        rhi::Pipeline pipeline_;
    };
} // namespace himalaya::passes
