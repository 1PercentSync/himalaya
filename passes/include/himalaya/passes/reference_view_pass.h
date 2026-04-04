#pragma once

/**
 * @file reference_view_pass.h
 * @brief ReferenceViewPass: path-traced reference view (passes layer).
 *
 * Dispatches the RT pipeline to accumulate one path-traced sample per pixel
 * per frame into the accumulation buffer (RGBA32F running average).
 * Also writes OIDN auxiliary images (albedo + normal) on bounce 0.
 *
 * Set 3 push descriptor layout: binding 0 accumulation (storage image),
 * binding 1 aux albedo (storage image), binding 2 aux normal (storage image),
 * binding 3 Sobol direction number SSBO.
 */

#include <himalaya/rhi/rt_pipeline.h>
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
     * @brief Path-traced reference view pass — RT pipeline dispatch with accumulation.
     *
     * Owns the RT pipeline and Set 3 push descriptor layout. Managed images
     * (accumulation + OIDN aux) are created by Renderer and passed via
     * FrameContext, consistent with other passes. CPU-side sample count drives
     * the running average; reset_accumulation() restarts convergence.
     */
    class ReferenceViewPass {
    public:
        /**
         * @brief One-time initialization: compile RT shaders, create pipeline.
         *
         * @param ctx              Vulkan context (RT properties, device).
         * @param rm               Resource manager (buffer access).
         * @param dm               Descriptor manager (global set layouts).
         * @param sc               Shader compiler (RT stage support).
         * @param sobol_buffer     Sobol direction number buffer (Set 3 binding 3).
         * @param blue_noise_index Bindless index of the 128x128 blue noise texture.
         */
        void setup(rhi::Context &ctx,
                   rhi::ResourceManager &rm,
                   rhi::DescriptorManager &dm,
                   rhi::ShaderCompiler &sc,
                   rhi::BufferHandle sobol_buffer,
                   uint32_t blue_noise_index);

        /**
         * @brief Register RG resource usage and provide the RT dispatch callback.
         *
         * @param rg  Render graph to add the pass to.
         * @param ctx Per-frame context with RG resource IDs and frame parameters.
         */
        void record(framework::RenderGraph &rg, const framework::FrameContext &ctx);

        /**
         * @brief Rebuild RT pipeline by recompiling shaders from disk.
         *
         * Caller must guarantee GPU is idle.
         */
        void rebuild_pipelines();

        /**
         * @brief Destroy pipeline and Set 3 layout.
         */
        void destroy();

        /**
         * @brief Resets accumulation — next frame overwrites instead of averaging.
         */
        void reset_accumulation();

        /** @brief Returns the number of samples accumulated so far. */
        [[nodiscard]] uint32_t sample_count() const;

    private:
        /**
         * @brief Create (or recreate) the RT pipeline.
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

        // ---- External resources (non-owning) ----

        /** @brief Sobol direction number buffer for Set 3 binding 3 push descriptor. */
        rhi::BufferHandle sobol_buffer_;

        /** @brief Bindless index of the 128x128 blue noise texture (push constant). */
        uint32_t blue_noise_index_ = 0;

        // ---- Owned resources ----

        /** @brief Set 3 push descriptor layout (bindings 0-2 storage image, binding 3 SSBO). */
        VkDescriptorSetLayout set3_layout_ = VK_NULL_HANDLE;

        /** @brief RT pipeline (pipeline + layout + SBT). */
        rhi::RTPipeline rt_pipeline_;

        // ---- Accumulation state ----

        /** @brief Number of samples accumulated so far (0 = next frame overwrites). */
        uint32_t sample_count_ = 0;

        /** @brief Monotonically increasing frame seed for temporal decorrelation. */
        uint32_t frame_seed_ = 0;
    };
} // namespace himalaya::passes
