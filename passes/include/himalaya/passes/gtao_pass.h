#pragma once

/**
 * @file gtao_pass.h
 * @brief GTAOPass: GTAO compute pass (passes layer).
 *
 * Dispatches the GTAO compute shader to produce per-pixel diffuse ambient
 * occlusion (R channel) and specular occlusion placeholder (G channel).
 * Output: ao_noisy (RG8, Set 3 push descriptor storage image).
 *
 * Reads depth (Set 2 binding 1) and normals (Set 2 binding 2) via the
 * global descriptor sets.  AO parameters are passed via push constants.
 */

#include <himalaya/rhi/pipeline.h>

#include <vulkan/vulkan.h>

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
     * @brief GTAO compute pass — structured horizon search with analytic integration.
     *
     * Non-MSAA (processes resolved depth/normal at 1x).
     * No on_sample_count_changed().
     */
    class GTAOPass {
    public:
        /**
         * @brief One-time initialization: create Set 3 layout, compile shader, create pipeline.
         *
         * @param ctx Vulkan context.
         * @param rm  Resource manager.
         * @param dm  Descriptor manager (for global set layouts).
         * @param sc  Shader compiler.
         */
        void setup(rhi::Context &ctx,
                   rhi::ResourceManager &rm,
                   rhi::DescriptorManager &dm,
                   rhi::ShaderCompiler &sc);

        /**
         * @brief Register RG resource usage and provide the compute dispatch callback.
         *
         * @param rg  Render graph to add the pass to.
         * @param ctx Per-frame context with RG resource IDs and AO config.
         */
        void record(framework::RenderGraph &rg, const framework::FrameContext &ctx) const;

        /**
         * @brief Rebuild pipeline by recompiling the shader from disk.
         *
         * Caller must guarantee GPU is idle.
         */
        void rebuild_pipelines();

        /**
         * @brief Destroy pipeline, Set 3 layout, and release owned resources.
         */
        void destroy();

    private:
        /**
         * @brief Create (or recreate) the compute pipeline.
         */
        void create_pipeline();

        // ---- Service pointers ----

        /** @brief Vulkan context. */
        rhi::Context *ctx_ = nullptr;

        /** @brief Resource manager. */
        rhi::ResourceManager *rm_ = nullptr;

        /** @brief Descriptor manager (global set layouts + per-frame sets). */
        rhi::DescriptorManager *dm_ = nullptr;

        /** @brief Shader compiler. */
        rhi::ShaderCompiler *sc_ = nullptr;

        // ---- Owned resources ----

        /** @brief Set 3 push descriptor layout (binding 0 = storage image). */
        VkDescriptorSetLayout set3_layout_ = VK_NULL_HANDLE;

        /** @brief GTAO compute pipeline. */
        rhi::Pipeline pipeline_;
    };
} // namespace himalaya::passes
