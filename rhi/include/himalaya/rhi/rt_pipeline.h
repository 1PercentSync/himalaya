#pragma once

/**
 * @file rt_pipeline.h
 * @brief Ray tracing pipeline creation and SBT management.
 */

#include <span>

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>

namespace himalaya::rhi {

    /**
     * @brief Description for creating a ray tracing pipeline.
     *
     * Shader modules are provided for each RT stage. The anyhit module is
     * optional (VK_NULL_HANDLE = hit group has no any-hit shader).
     * SBT layout: raygen(1 entry) + miss(2 entries: environment + shadow) +
     * hit(1 entry: closest-hit + optional any-hit).
     */
    struct RTPipelineDesc {
        /** @brief Raygen shader module. */
        VkShaderModule raygen = VK_NULL_HANDLE;

        /** @brief Environment miss shader module. */
        VkShaderModule miss = VK_NULL_HANDLE;

        /** @brief Shadow miss shader module. */
        VkShaderModule shadow_miss = VK_NULL_HANDLE;

        /** @brief Closest-hit shader module. */
        VkShaderModule closesthit = VK_NULL_HANDLE;

        /**
         * @brief Any-hit shader module for alpha test and stochastic alpha.
         *
         * VK_NULL_HANDLE = no any-hit shader in the hit group.
         * When provided, the hit group contains both closest-hit and any-hit.
         */
        VkShaderModule anyhit = VK_NULL_HANDLE;

        /** @brief Maximum ray recursion depth (1 for raygen→closesthit→shadow_miss). */
        uint32_t max_recursion_depth = 1;

        /** @brief Descriptor set layouts for the pipeline layout. */
        std::span<const VkDescriptorSetLayout> descriptor_set_layouts;

        /** @brief Push constant ranges for the pipeline layout. */
        std::span<const VkPushConstantRange> push_constant_ranges;
    };

    /**
     * @brief Ray tracing pipeline with bound SBT lifetime.
     *
     * Owns a VkPipeline, VkPipelineLayout, and the SBT buffer. The SBT
     * regions are pre-computed for direct use with vkCmdTraceRaysKHR.
     * destroy() must be called before the device is destroyed.
     */
    struct RTPipeline {
        /** @brief Vulkan ray tracing pipeline. */
        VkPipeline pipeline = VK_NULL_HANDLE;

        /** @brief Pipeline layout (descriptor set layouts + push constant ranges). */
        VkPipelineLayout layout = VK_NULL_HANDLE;

        /** @brief SBT buffer containing raygen, miss, and hit shader group handles. */
        VkBuffer sbt_buffer = VK_NULL_HANDLE;

        /** @brief VMA allocation for the SBT buffer. */
        VmaAllocation sbt_allocation = VK_NULL_HANDLE;

        /** @brief SBT region for the raygen shader group (1 entry). */
        VkStridedDeviceAddressRegionKHR raygen_region{};

        /** @brief SBT region for miss shader groups (2 entries: env + shadow). */
        VkStridedDeviceAddressRegionKHR miss_region{};

        /** @brief SBT region for hit shader groups (1 entry: chit + optional ahit). */
        VkStridedDeviceAddressRegionKHR hit_region{};

        /** @brief Empty callable region (not used). */
        VkStridedDeviceAddressRegionKHR callable_region{};

        /**
         * @brief Destroys the pipeline, layout, and SBT buffer.
         * @param device    Logical device that owns the pipeline.
         * @param allocator VMA allocator that owns the SBT buffer.
         */
        void destroy(VkDevice device, VmaAllocator allocator);
    };

    /**
     * @brief Creates a ray tracing pipeline and builds the SBT.
     *
     * Creates shader groups (raygen, 2x miss, 1x hit group with chit + optional ahit),
     * calls vkCreateRayTracingPipelinesKHR, queries shader group handles via
     * vkGetRayTracingShaderGroupHandlesKHR, and writes them into an aligned SBT buffer.
     *
     * @param device    Logical device.
     * @param allocator VMA allocator for SBT buffer allocation.
     * @param desc      Pipeline configuration (shader modules, layouts, push constants).
     * @param rt_props  RT pipeline properties (handle size, alignment requirements).
     * @return Created pipeline with pre-computed SBT regions.
     */
    [[nodiscard]] RTPipeline create_rt_pipeline(
        VkDevice device, VmaAllocator allocator,
        const RTPipelineDesc &desc,
        const VkPhysicalDeviceRayTracingPipelinePropertiesKHR &rt_props);

} // namespace himalaya::rhi
