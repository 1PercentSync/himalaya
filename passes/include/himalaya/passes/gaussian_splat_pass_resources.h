#pragma once

/**
 * @file gaussian_splat_pass_resources.h
 * @brief Renderer-lifetime Gaussian Splatting pass shared resources.
 */

#include <vulkan/vulkan.h>

namespace himalaya::rhi {
    class Context;
}

namespace himalaya::passes {
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

    private:
        /** @brief Vulkan context used to own the descriptor layout. */
        rhi::Context *ctx_ = nullptr;

        /** @brief Persistent GS Set 3 descriptor layout for static and work storage buffers. */
        VkDescriptorSetLayout descriptor_set_layout_ = VK_NULL_HANDLE;
    };
} // namespace himalaya::passes
