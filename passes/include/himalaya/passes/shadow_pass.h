#pragma once

/**
 * @file shadow_pass.h
 * @brief CSM shadow pass (passes layer).
 *
 * Renders scene depth from the directional light's perspective into a
 * 2D array shadow map (one layer per cascade). Self-manages shadow map
 * resources (not RG-managed); each frame imports into the Render Graph.
 *
 * Two pipelines: Opaque (depth-only, no FS) and Mask (alpha test + discard).
 */

#include <himalaya/framework/scene_data.h>
#include <himalaya/rhi/pipeline.h>
#include <himalaya/rhi/types.h>

#include <array>

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
    /** @brief Default shadow map resolution per cascade (pixels). */
    inline constexpr uint32_t kDefaultShadowResolution = 2048;

    /**
     * @brief CSM shadow pass — renders depth from light space into a 2D array shadow map.
     *
     * Not MSAA-aware (shadow map is always 1x). No on_sample_count_changed().
     * Resolution changes via on_resolution_changed() (destroys + recreates resources).
     * Cascade count is a pure rendering parameter — does not affect resources.
     */
    class ShadowPass {
    public:
        /**
         * @brief One-time initialization: create shadow map resources, compile shaders, create pipelines.
         *
         * @param ctx Vulkan context.
         * @param rm  Resource manager for image/view creation.
         * @param dm  Descriptor manager for set binding.
         * @param sc  Shader compiler for GLSL -> SPIR-V.
         */
        void setup(rhi::Context &ctx,
                   rhi::ResourceManager &rm,
                   rhi::DescriptorManager &dm,
                   rhi::ShaderCompiler &sc);

        /**
         * @brief Register RG resource usage and provide the execute callback.
         *
         * Imports shadow map into RG, adds a single pass that loops over active
         * cascades internally (one begin/end rendering per cascade layer).
         *
         * @param rg  Render graph to add the pass to.
         * @param ctx Per-frame context with scene data and shadow draw groups.
         */
        void record(framework::RenderGraph &rg, const framework::FrameContext &ctx) const;

        /**
         * @brief Destroy all owned resources (shadow map, layer views, pipelines).
         */
        void destroy();

        /**
         * @brief Rebuild shadow map resources at a new resolution.
         *
         * Caller must guarantee GPU is idle before calling (vkQueueWaitIdle).
         * Destroys old image + layer views, creates new ones. Pipeline is unchanged.
         *
         * @param resolution New shadow map resolution (width = height = resolution).
         */
        void on_resolution_changed(uint32_t resolution);

        /**
         * @brief Rebuild pipelines by recompiling shaders from disk.
         *
         * Caller must guarantee GPU is idle before calling (vkQueueWaitIdle).
         */
        void rebuild_pipelines();

        /**
         * @brief Returns the shadow map image handle (for Set 2 descriptor updates).
         */
        [[nodiscard]] rhi::ImageHandle shadow_map_image() const { return shadow_map_image_; }

        /**
         * @brief Returns the current shadow map resolution (width = height).
         */
        [[nodiscard]] uint32_t resolution() const { return resolution_; }

    private:
        /**
         * @brief Create shadow map image + per-layer views at the given resolution.
         *
         * @param resolution Shadow map resolution (width = height).
         */
        void create_shadow_map(uint32_t resolution);

        /**
         * @brief Destroy shadow map image + per-layer views.
         */
        void destroy_shadow_map();

        /**
         * @brief Create (or recreate) opaque and mask graphics pipelines.
         */
        void create_pipelines();

        // ---- Service pointers (set in setup) ----

        /** @brief Vulkan context. */
        rhi::Context *ctx_ = nullptr;

        /** @brief Resource manager for image/buffer/view operations. */
        rhi::ResourceManager *rm_ = nullptr;

        /** @brief Descriptor manager for set binding during recording. */
        rhi::DescriptorManager *dm_ = nullptr;

        /** @brief Shader compiler for GLSL -> SPIR-V compilation. */
        rhi::ShaderCompiler *sc_ = nullptr;

        // ---- Configuration ----

        /** @brief Current shadow map resolution (width = height). */
        uint32_t resolution_ = 0;

        // ---- Owned resources ----

        /** @brief Shadow map 2D array image (D32Sfloat, kMaxShadowCascades layers). */
        rhi::ImageHandle shadow_map_image_;

        /** @brief Per-cascade layer views for rendering into individual layers. */
        std::array<VkImageView, framework::kMaxShadowCascades> layer_views_{};

        /** @brief Opaque geometry pipeline (depth-only, no fragment shader). */
        rhi::Pipeline opaque_pipeline_;

        /** @brief Alpha mask geometry pipeline (alpha test + discard). */
        rhi::Pipeline mask_pipeline_;
    };
} // namespace himalaya::passes
