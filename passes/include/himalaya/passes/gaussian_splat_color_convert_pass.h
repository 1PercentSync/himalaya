#pragma once

/**
 * @file gaussian_splat_color_convert_pass.h
 * @brief Gaussian Splatting color-space conversion pass.
 */

#include <himalaya/rhi/pipeline.h>
#include <himalaya/rhi/types.h>

#include <vulkan/vulkan.h>

namespace himalaya::rhi {
    class Context;
    class ResourceManager;
    class ShaderCompiler;
}

namespace himalaya::framework {
    class RenderGraph;
    struct RGResourceId;
}

namespace himalaya::passes {
    /** @brief Workgroup width/height for GS color conversion compute dispatch. */
    inline constexpr uint32_t kGaussianSplatColorConvertWorkgroupSize = 8;

    /**
     * @brief Converts GS composition color to linear display-referred output.
     *
     * The pass is used for KHR `srgb_rec709_display` scenes. RGB is decoded from
     * sRGB to linear while alpha is preserved unchanged. Descriptor inputs are
     * pushed per dispatch so the pass does not consume Set 2 render target slots.
     */
    class GaussianSplatColorConvertPass {
    public:
        /**
         * @brief Creates the compute pipeline and push descriptor layout.
         * @param ctx Vulkan context used for pipeline and layout creation.
         * @param rm Resource manager used to resolve RG image handles.
         * @param sc Shader compiler used for runtime GLSL compilation.
         * @param sampler Sampler used for reading the composition image.
         */
        void setup(rhi::Context &ctx,
                   rhi::ResourceManager &rm,
                   rhi::ShaderCompiler &sc,
                   rhi::SamplerHandle sampler);

        /**
         * @brief Registers the sRGB composition to linear target conversion pass.
         * @param rg Render graph to add the compute pass to.
         * @param source Composition image in `srgb_rec709_display` color space.
         * @param destination Linear display-referred output image.
         */
        void record(framework::RenderGraph &rg,
                    framework::RGResourceId source,
                    framework::RGResourceId destination) const;

        /** @brief Rebuilds the compute pipeline by recompiling the shader from disk. */
        void rebuild_pipelines();

        /** @brief Destroys the compute pipeline and descriptor layout. */
        void destroy();

    private:
        /** @brief Creates or recreates the compute pipeline. */
        void create_pipeline();

        /** @brief Vulkan context used for pipeline and layout ownership. */
        rhi::Context *ctx_ = nullptr;

        /** @brief Resource manager used to resolve image handles at record time. */
        rhi::ResourceManager *rm_ = nullptr;

        /** @brief Shader compiler used for conversion shader compilation. */
        rhi::ShaderCompiler *sc_ = nullptr;

        /** @brief Sampler used for the source composition image. */
        rhi::SamplerHandle sampler_{};

        /** @brief Push descriptor layout: binding 0 sampled input, binding 1 storage output. */
        VkDescriptorSetLayout push_descriptor_layout_ = VK_NULL_HANDLE;

        /** @brief Compute pipeline for RGB sRGB decode with alpha preservation. */
        rhi::Pipeline pipeline_{};
    };
} // namespace himalaya::passes
