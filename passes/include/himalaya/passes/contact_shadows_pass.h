#pragma once

/**
 * @file contact_shadows_pass.h
 * @brief ContactShadowsPass: screen-space contact shadows compute pass (passes layer).
 *
 * Dispatches the contact shadows compute shader to produce a per-pixel
 * shadow mask for the primary directional light.  Reads depth from
 * Set 2 binding 1 (rt_depth_resolved) and writes contact_shadow_mask
 * (R8, Set 3 push storage).
 *
 * Light direction is read from LightBuffer SSBO (Set 0, Binding 1) in
 * the shader.  Config parameters are passed via push constants.
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
     * @brief Screen-space contact shadows compute pass.
     *
     * Non-MSAA (processes resolved depth at 1x).
     * No on_sample_count_changed().
     */
    class ContactShadowsPass {
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
         * @param ctx Per-frame context with RG resource IDs and contact shadow config.
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

        /** @brief Contact shadows compute pipeline. */
        rhi::Pipeline pipeline_;
    };
} // namespace himalaya::passes
