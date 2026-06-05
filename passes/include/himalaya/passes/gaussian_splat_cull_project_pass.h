#pragma once

/**
 * @file gaussian_splat_cull_project_pass.h
 * @brief Gaussian Splatting cull/project compute pass skeleton.
 */

#include <himalaya/passes/gaussian_splat_pass_resources.h>
#include <himalaya/passes/gs_push_constants.h>
#include <himalaya/rhi/pipeline.h>

#include <cstdint>

#include <vulkan/vulkan.h>

namespace himalaya::rhi {
    class Context;
    class DescriptorManager;
    class ShaderCompiler;
}

namespace himalaya::framework {
    struct GaussianSplatGpuScene;
    class RenderGraph;
}

namespace himalaya::passes {
    /** @brief Number of splats processed by one cull/project compute workgroup. */
    inline constexpr uint32_t kGaussianSplatCullProjectWorkgroupSize = 256;

    /**
     * @brief Dispatches the GS cull/project compute shader.
     *
     * The pass owns only the compute pipeline and pipeline layout. Persistent
     * scene buffers and the GS Set 3 descriptor set are owned by the renderer-held
     * GaussianSplatSceneBuilder and imported through GaussianSplatGraphResources.
     */
    class GaussianSplatCullProjectPass {
    public:
        /**
         * @brief Creates the compute pipeline using global Set 0-2 and persistent GS Set 3.
         * @param ctx Vulkan context used to create the pipeline.
         * @param dm Descriptor manager providing global descriptor set layouts and sets.
         * @param sc Shader compiler used for runtime GLSL compilation.
         * @param gs_set3_layout Persistent GS Set 3 descriptor set layout.
         */
        void setup(rhi::Context &ctx,
                   rhi::DescriptorManager &dm,
                   rhi::ShaderCompiler &sc,
                   VkDescriptorSetLayout gs_set3_layout);

        /**
         * @brief Registers the cull/project dispatch in the render graph.
         * @param rg Render graph to add the compute pass to.
         * @param resources Per-frame imported GS scene buffer resources.
         * @param scene Uploaded GS scene with a persistent Set 3 descriptor set.
         * @param frame_index Current frame-in-flight index for Set 0-2 binding.
         * @param push_constants Per-frame GS scalar state for the shader.
         */
        void record(framework::RenderGraph &rg,
                    const GaussianSplatGraphResources &resources,
                    const framework::GaussianSplatGpuScene &scene,
                    uint32_t frame_index,
                    const GSPushConstants &push_constants) const;

        /**
         * @brief Rebuilds the compute pipeline by recompiling the shader from disk.
         *
         * Caller must guarantee the GPU is idle before rebuilding.
         */
        void rebuild_pipelines();

        /** @brief Destroys the compute pipeline and clears service pointers. */
        void destroy();

    private:
        /** @brief Creates or recreates the compute pipeline. */
        void create_pipeline();

        /** @brief Vulkan context used for pipeline creation and destruction. */
        rhi::Context *ctx_ = nullptr;

        /** @brief Descriptor manager used for global descriptor layouts and per-frame sets. */
        rhi::DescriptorManager *dm_ = nullptr;

        /** @brief Shader compiler used for cull/project shader compilation. */
        rhi::ShaderCompiler *sc_ = nullptr;

        /** @brief Persistent GS Set 3 descriptor set layout used by the pipeline layout. */
        VkDescriptorSetLayout gs_set3_layout_ = VK_NULL_HANDLE;

        /** @brief Compute pipeline for GS cull/project. */
        rhi::Pipeline pipeline_{};
    };
} // namespace himalaya::passes
