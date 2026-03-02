#pragma once

/**
 * @file pipeline.h
 * @brief Graphics pipeline creation with Dynamic Rendering and Extended Dynamic State.
 */

#include <vector>

#include <vulkan/vulkan.h>

namespace himalaya::rhi {
    /**
     * @brief Description for creating a graphics pipeline.
     *
     * Configures shader stages, vertex input, color/depth attachment formats
     * (for Dynamic Rendering), and pipeline layout bindings.
     * Viewport, scissor, cull mode, and depth state are always dynamic
     * (Extended Dynamic State) and not specified here.
     */
    struct GraphicsPipelineDesc {
        /** @brief Vertex shader module (must be valid). */
        VkShaderModule vertex_shader = VK_NULL_HANDLE;

        /** @brief Fragment shader module (must be valid). */
        VkShaderModule fragment_shader = VK_NULL_HANDLE;

        /** @brief Color attachment formats for Dynamic Rendering. */
        std::vector<VkFormat> color_formats;

        /** @brief Depth attachment format (VK_FORMAT_UNDEFINED = no depth). */
        VkFormat depth_format = VK_FORMAT_UNDEFINED;

        /** @brief Vertex input binding descriptions (empty for hardcoded vertices). */
        std::vector<VkVertexInputBindingDescription> vertex_bindings;

        /** @brief Vertex input attribute descriptions (empty for hardcoded vertices). */
        std::vector<VkVertexInputAttributeDescription> vertex_attributes;

        /** @brief Primitive topology. */
        VkPrimitiveTopology topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

        /** @brief Enable alpha blending on the first color attachment. */
        bool blend_enable = false;

        /** @brief Descriptor set layouts for the pipeline layout. */
        std::vector<VkDescriptorSetLayout> descriptor_set_layouts;

        /** @brief Push constant ranges for the pipeline layout. */
        std::vector<VkPushConstantRange> push_constant_ranges;
    };

    /**
     * @brief Holds a VkPipeline and its associated VkPipelineLayout.
     *
     * Both are created together because they have a 1:1 relationship in M1.
     * Lifetime is managed explicitly via destroy().
     */
    struct Pipeline {
        /** @brief Vulkan graphics pipeline. */
        VkPipeline pipeline = VK_NULL_HANDLE;

        /** @brief Pipeline layout (descriptor set layouts + push constant ranges). */
        VkPipelineLayout layout = VK_NULL_HANDLE;

        /**
         * @brief Destroys the pipeline and its layout.
         * @param device Logical device that owns these objects.
         */
        void destroy(VkDevice device) const;
    };

    /**
     * @brief Creates a graphics pipeline with Dynamic Rendering and Extended Dynamic State.
     *
     * Dynamic states (set at command buffer recording time, not baked into the pipeline):
     * - Viewport and scissor
     * - Cull mode and front face
     * - Depth test enable, write enable, and compare op
     *
     * @param device Logical device.
     * @param desc   Pipeline configuration.
     * @return Created pipeline and layout.
     */
    [[nodiscard]] Pipeline create_graphics_pipeline(VkDevice device, const GraphicsPipelineDesc &desc);
} // namespace himalaya::rhi
