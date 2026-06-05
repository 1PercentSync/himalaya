#pragma once

/**
 * @file gaussian_splat_draw_pass.h
 * @brief Gaussian Splatting hardware raster composition pass.
 */

#include <himalaya/passes/gaussian_splat_pass_resources.h>
#include <himalaya/passes/gs_push_constants.h>
#include <himalaya/rhi/pipeline.h>

#include <cstdint>

#include <vulkan/vulkan.h>

namespace himalaya::rhi {
    class Context;
    class DescriptorManager;
    class ResourceManager;
    class ShaderCompiler;
}

namespace himalaya::framework {
    struct GaussianSplatGpuScene;
    class RenderGraph;
    struct RGResourceId;
}

namespace himalaya::passes {
    /** @brief Floating-point composition format used by the GS hardware raster path. */
    inline constexpr VkFormat kGaussianSplatCompositionFormat = VK_FORMAT_R16G16B16A16_SFLOAT;

    /**
     * @brief Owns the GS instanced-quad graphics pipeline and composition pass recording.
     *
     * The pass writes a viewport-sized floating-point composition target with
     * front-to-back premultiplied-under blending. Scene buffers and the
     * persistent GS Set 3 descriptor set remain owned by the renderer-held GS
     * scene resource owner.
     */
    class GaussianSplatDrawPass {
    public:
        /**
         * @brief Creates the graphics pipeline using global Set 0-2 and persistent GS Set 3.
         * @param ctx Vulkan context used to create the pipeline.
         * @param rm Resource manager used to resolve the composition target image.
         * @param dm Descriptor manager providing global descriptor set layouts and sets.
         * @param sc Shader compiler used for runtime GLSL compilation.
         * @param gs_set3_layout Persistent GS Set 3 descriptor set layout.
         */
        void setup(rhi::Context &ctx,
                   rhi::ResourceManager &rm,
                   rhi::DescriptorManager &dm,
                   rhi::ShaderCompiler &sc,
                   VkDescriptorSetLayout gs_set3_layout);

        /**
         * @brief Registers the GS composition render pass.
         *
         * Uses the cull/project-written VkDrawIndirectCommand so draw range is
         * driven by visible_count while fixed command fields remain CPU-initialized.
         *
         * @param rg Render graph to add the graphics pass to.
         * @param composition_target Per-frame RG image ID for the GS composition target.
         * @param resources Per-frame imported GS scene buffer resources.
         * @param scene Uploaded GS scene with a persistent Set 3 descriptor set.
         * @param frame_index Current frame-in-flight index for Set 0-2 binding.
         * @param push_constants Per-frame GS scalar state shared by draw shaders.
         */
        void record(framework::RenderGraph &rg,
                    framework::RGResourceId composition_target,
                    const GaussianSplatGraphResources &resources,
                    const framework::GaussianSplatGpuScene &scene,
                    uint32_t frame_index,
                    const GSPushConstants &push_constants) const;

        /**
         * @brief Rebuilds the graphics pipeline by recompiling shaders from disk.
         *
         * Caller must guarantee the GPU is idle before rebuilding.
         */
        void rebuild_pipelines();

        /** @brief Destroys the graphics pipeline and clears service pointers. */
        void destroy();

    private:
        /** @brief Creates or recreates the graphics pipeline. */
        void create_pipeline();

        /** @brief Vulkan context used for pipeline creation and destruction. */
        rhi::Context *ctx_ = nullptr;

        /** @brief Resource manager used for composition image access during recording. */
        rhi::ResourceManager *rm_ = nullptr;

        /** @brief Descriptor manager used for global descriptor layouts and per-frame sets. */
        rhi::DescriptorManager *dm_ = nullptr;

        /** @brief Shader compiler used for draw shader compilation. */
        rhi::ShaderCompiler *sc_ = nullptr;

        /** @brief Persistent GS Set 3 descriptor set layout used by the pipeline layout. */
        VkDescriptorSetLayout gs_set3_layout_ = VK_NULL_HANDLE;

        /** @brief Graphics pipeline for GS instanced quad composition. */
        rhi::Pipeline pipeline_{};
    };
} // namespace himalaya::passes
