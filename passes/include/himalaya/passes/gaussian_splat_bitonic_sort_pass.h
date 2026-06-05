#pragma once

/**
 * @file gaussian_splat_bitonic_sort_pass.h
 * @brief Gaussian Splatting bitonic sort compute pass.
 */

#include <himalaya/passes/gaussian_splat_pass_resources.h>
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
}

namespace himalaya::passes {
    /** @brief Number of sort entries processed by one bitonic sort compute workgroup. */
    inline constexpr uint32_t kGaussianSplatBitonicSortWorkgroupSize = 256;

    /**
     * @brief Push constant layout for one bitonic sort dispatch step.
     *
     * Kept separate from GSPushConstants because stage_k and step_j are local
     * implementation details of the bitonic sort network, not shared GS render
     * parameters.
     */
    struct GaussianSplatBitonicSortPushConstants {
        uint32_t sort_capacity; ///< Power-of-two number of sort entries processed by the shader.
        uint32_t stage_k; ///< Current bitonic merge stage size.
        uint32_t step_j; ///< Current compare-and-swap partner distance.
        uint32_t padding; ///< Explicit padding to keep the push constant block 16-byte aligned.
    };

    static_assert(sizeof(GaussianSplatBitonicSortPushConstants) == 16);

    /**
     * @brief Dispatches the in-place GS bitonic sort correctness baseline.
     *
     * The pass records the complete bitonic stage/step sequence as multiple
     * dispatches inside a single RenderGraph pass. This avoids creating hundreds
     * of RG passes for 1M+ splat scenes. The pass therefore inserts explicit
     * compute-to-compute buffer barriers between dispatch steps for the primary
     * sort_entries buffer. Cross-pass hazards remain owned by RenderGraph.
     *
     * The Phase 3.0 bitonic baseline intentionally does not use the scratch sort
     * buffer. Results always remain in the primary sort_entries buffer; scratch
     * is reserved for the later out-of-place radix / ping-pong scatter path.
     */
    class GaussianSplatBitonicSortPass {
    public:
        /**
         * @brief Creates the compute pipeline using global Set 0-2 and persistent GS Set 3.
         * @param ctx Vulkan context used to create the pipeline.
         * @param rm Resource manager used to resolve the primary sort-entry buffer for manual barriers.
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
         * @brief Registers the complete capacity bitonic sort sequence in the render graph.
         * @param rg Render graph to add the compute pass to.
         * @param resources Per-frame imported GS scene buffer resources.
         * @param scene Uploaded GS scene with a persistent Set 3 descriptor set.
         * @param frame_index Current frame-in-flight index for Set 0-2 binding.
         */
        void record(framework::RenderGraph &rg,
                    const GaussianSplatGraphResources &resources,
                    const framework::GaussianSplatGpuScene &scene,
                    uint32_t frame_index) const;

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

        /** @brief Resource manager used for pass-internal sort-entry buffer barriers. */
        rhi::ResourceManager *rm_ = nullptr;

        /** @brief Descriptor manager used for global descriptor layouts and per-frame sets. */
        rhi::DescriptorManager *dm_ = nullptr;

        /** @brief Shader compiler used for bitonic sort shader compilation. */
        rhi::ShaderCompiler *sc_ = nullptr;

        /** @brief Persistent GS Set 3 descriptor set layout used by the pipeline layout. */
        VkDescriptorSetLayout gs_set3_layout_ = VK_NULL_HANDLE;

        /** @brief Compute pipeline for GS bitonic sort. */
        rhi::Pipeline pipeline_{};
    };
} // namespace himalaya::passes
