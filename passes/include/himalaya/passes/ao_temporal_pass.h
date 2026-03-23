#pragma once

/**
 * @file ao_temporal_pass.h
 * @brief AOTemporalPass: AO temporal filter compute pass (passes layer).
 *
 * Blends current-frame GTAO output with the previous frame's filtered result
 * using reprojection and three-layer rejection (UV validity, depth consistency,
 * neighborhood clamp).
 *
 * Input:  ao_blurred (Set 3 push sampled), ao_history (Set 3 push sampled),
 *         depth_prev (Set 3 push sampled), depth (Set 2 binding 1).
 * Output: ao_filtered (RG8, Set 3 push storage image).
 */

#include <himalaya/rhi/pipeline.h>
#include <himalaya/rhi/types.h>

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
     * @brief AO temporal filter compute pass — reprojection + rejection + blend.
     *
     * Non-MSAA (processes resolved AO/depth at 1x).
     * No on_sample_count_changed().
     */
    class AOTemporalPass {
    public:
        /**
         * @brief One-time initialization: create Set 3 layout, compile shader, create pipeline.
         *
         * @param ctx             Vulkan context.
         * @param rm              Resource manager.
         * @param dm              Descriptor manager (for global set layouts).
         * @param sc              Shader compiler.
         * @param nearest_sampler Nearest-clamp sampler for push descriptor sampled inputs.
         */
        void setup(rhi::Context &ctx,
                   rhi::ResourceManager &rm,
                   rhi::DescriptorManager &dm,
                   rhi::ShaderCompiler &sc,
                   rhi::SamplerHandle nearest_sampler);

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

        // ---- Configuration ----

        /** @brief Nearest-clamp sampler for all push descriptor sampled inputs. */
        rhi::SamplerHandle nearest_sampler_;

        // ---- Owned resources ----

        /** @brief Set 3 push descriptor layout (4 bindings: 1 storage + 3 sampled). */
        VkDescriptorSetLayout set3_layout_ = VK_NULL_HANDLE;

        /** @brief AO temporal filter compute pipeline. */
        rhi::Pipeline pipeline_;
    };
} // namespace himalaya::passes
