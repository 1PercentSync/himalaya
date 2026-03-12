#pragma once

/**
 * @file tonemapping_pass.h
 * @brief TonemappingPass: fullscreen post-processing pass (Layer 2).
 *
 * Reads the HDR color buffer (via Set 2 binding 0) and writes the
 * tonemapped result to the swapchain image. Step 4a is passthrough;
 * Step 4b upgrades to ACES tonemapping with exposure control.
 *
 * Non-MSAA pass: always renders at 1x, processes resolved HDR output.
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
     * @brief Tonemapping pass — fullscreen fragment shader reading HDR color.
     *
     * Non-MSAA pass: no on_sample_count_changed(). Swapchain format is
     * provided at setup() time (physical device negotiation result).
     */
    class TonemappingPass {
    public:
        /**
         * @brief One-time initialization: compile shaders, create pipeline, store service pointers.
         *
         * @param ctx              Vulkan context.
         * @param rm               Resource manager.
         * @param dm               Descriptor manager (for Set 2 hdr_color binding).
         * @param sc               Shader compiler.
         * @param swapchain_format Swapchain surface format (pipeline color attachment format).
         */
        void setup(rhi::Context &ctx,
                   rhi::ResourceManager &rm,
                   rhi::DescriptorManager &dm,
                   rhi::ShaderCompiler &sc,
                   VkFormat swapchain_format);

        /**
         * @brief Update resolution-dependent state.
         *
         * Updates Set 2 binding 0 with the current hdr_color backing image
         * so the tonemapping shader samples the correct (possibly resized) target.
         *
         * @param width  New render width.
         * @param height New render height.
         */
        void on_resize(uint32_t width, uint32_t height);

        /**
         * @brief Register RG resource usage and provide the execute callback.
         *
         * @param rg  Render graph to add the pass to.
         * @param ctx Per-frame context with RG resource IDs.
         */
        void record(framework::RenderGraph &rg, const framework::FrameContext &ctx) const;

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

        /** @brief Descriptor manager for Set 2 binding. */
        rhi::DescriptorManager *dm_ = nullptr;

        /** @brief Shader compiler. */
        rhi::ShaderCompiler *sc_ = nullptr;

        // ---- Configuration ----

        /** @brief Swapchain surface format (baked into pipeline). */
        VkFormat swapchain_format_ = VK_FORMAT_UNDEFINED;

        // ---- Owned resources ----

        /** @brief Tonemapping fullscreen pipeline. */
        rhi::Pipeline pipeline_;
    };
} // namespace himalaya::passes
