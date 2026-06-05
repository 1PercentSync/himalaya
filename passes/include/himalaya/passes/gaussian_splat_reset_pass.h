#pragma once

/**
 * @file gaussian_splat_reset_pass.h
 * @brief Gaussian Splatting work-buffer reset pass.
 */

#include <himalaya/passes/gaussian_splat_pass_resources.h>

namespace himalaya::rhi {
    class ResourceManager;
}

namespace himalaya::framework {
    class RenderGraph;
}

namespace himalaya::passes {
    /**
     * @brief Resets per-frame GS work buffers before cull/project.
     *
     * This pass has no pipeline. It uses transfer fill commands to reset the
     * visible counter, sort entries, and draw-indirect instance count. The fixed
     * VkDrawIndirectCommand fields are initialized by the GS scene builder when
     * the indirect buffer is created.
     */
    class GaussianSplatResetPass {
    public:
        /**
         * @brief Stores service pointers required for buffer handle resolution.
         * @param rm Resource manager used to resolve imported RG buffers.
         */
        void setup(rhi::ResourceManager &rm);

        /**
         * @brief Registers the reset pass in the render graph.
         * @param rg Render graph to add the reset pass to.
         * @param resources Per-frame RG resource IDs for imported GS scene buffers.
         */
        void record(framework::RenderGraph &rg,
                    const GaussianSplatGraphResources &resources) const;

        /**
         * @brief Clears stored service pointers.
         */
        void destroy();

    private:
        /** @brief Resource manager used to resolve buffer handles at execute time. */
        rhi::ResourceManager *rm_ = nullptr;
    };
} // namespace himalaya::passes
