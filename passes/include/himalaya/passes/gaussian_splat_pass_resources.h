#pragma once

/**
 * @file gaussian_splat_pass_resources.h
 * @brief Renderer-lifetime Gaussian Splatting pass shared resources.
 */

#include <himalaya/framework/render_graph.h>

#include <vulkan/vulkan.h>

namespace himalaya::rhi {
    class Context;
}

namespace himalaya::framework {
    struct GaussianSplatGpuScene;
}

namespace himalaya::passes {
    /**
     * @brief Per-frame RenderGraph resource IDs for all persistent GS scene buffers.
     *
     * The GS path imports each static/work buffer once per frame and shares these
     * IDs across reset, cull/project, sort, and draw passes so RenderGraph can
     * observe buffer hazards between the passes.
     */
    struct GaussianSplatGraphResources {
        framework::RGResourceId position_radius;
        framework::RGResourceId covariance_opacity;
        framework::RGResourceId sh_coefficients;
        framework::RGResourceId visible_count;
        framework::RGResourceId projected_data;
        framework::RGResourceId sort_entries;
        framework::RGResourceId sort_entries_scratch;
        framework::RGResourceId indirect_draw;
    };

    /**
     * @brief Owns renderer-lifetime shared resources for GS passes.
     *
     * Mirrors the PT pass ownership model: descriptor layout state used by GS
     * pipelines belongs to a pass-layer renderer-lifetime owner, while scene
     * builders only manage scene-dependent buffers and descriptor contents.
     */
    class GaussianSplatPassResources {
    public:
        /**
         * @brief Creates the persistent GS Set 3 descriptor layout.
         * @param ctx Vulkan context used to create descriptor set layouts.
         */
        void setup(rhi::Context &ctx);

        /**
         * @brief Destroys the persistent GS Set 3 descriptor layout.
         */
        void destroy();

        /** @brief Returns the persistent GS Set 3 descriptor set layout. */
        [[nodiscard]] VkDescriptorSetLayout descriptor_set_layout() const;

        /**
         * @brief Imports all persistent GS scene buffers for one frame.
         *
         * The returned RGResourceIds must be shared by all GS passes in the frame;
         * callers must not re-import the same GS buffers independently.
         *
         * @param rg Render graph for the current frame.
         * @param scene Uploaded GS scene with valid static and work buffers.
         * @return RenderGraph resource IDs for the scene buffers.
         */
        [[nodiscard]] GaussianSplatGraphResources import_scene_resources(
            framework::RenderGraph &rg,
            const framework::GaussianSplatGpuScene &scene) const;

    private:
        /** @brief Vulkan context used to own the descriptor layout. */
        rhi::Context *ctx_ = nullptr;

        /** @brief Persistent GS Set 3 descriptor layout for static and work storage buffers. */
        VkDescriptorSetLayout descriptor_set_layout_ = VK_NULL_HANDLE;
    };
} // namespace himalaya::passes
