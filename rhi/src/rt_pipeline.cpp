#include <himalaya/rhi/rt_pipeline.h>
#include <himalaya/rhi/context.h>

#include <spdlog/spdlog.h>

#include <array>
#include <vector>

namespace himalaya::rhi {
    /// Aligns a value up to the given power-of-two alignment.
    static uint32_t align_up(const uint32_t value, const uint32_t alignment) {
        return (value + alignment - 1) & ~(alignment - 1);
    }

    RTPipeline create_rt_pipeline(const Context &ctx, const RTPipelineDesc &desc) {
        VkDevice device = ctx.device;
        VmaAllocator allocator = ctx.allocator;
        // --- Shader stages ---
        // Order: raygen(0), miss(1), shadow_miss(2), closesthit(3), [anyhit(4)]

        std::vector<VkPipelineShaderStageCreateInfo> stages;
        stages.reserve(5);

        // ReSharper disable once CppParameterMayBeConst
        auto add_stage = [&](const VkShaderStageFlagBits stage, VkShaderModule module) -> uint32_t {
            VkPipelineShaderStageCreateInfo info{};
            info.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            info.stage = stage;
            info.module = module;
            info.pName = "main";
            const auto index = static_cast<uint32_t>(stages.size());
            stages.push_back(info);
            return index;
        };

        const uint32_t raygen_stage = add_stage(VK_SHADER_STAGE_RAYGEN_BIT_KHR, desc.raygen);
        const uint32_t miss_stage = add_stage(VK_SHADER_STAGE_MISS_BIT_KHR, desc.miss);
        const uint32_t shadow_miss_stage = add_stage(VK_SHADER_STAGE_MISS_BIT_KHR, desc.shadow_miss);
        const uint32_t closesthit_stage = add_stage(VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR, desc.closesthit);

        uint32_t anyhit_stage = VK_SHADER_UNUSED_KHR;
        if (desc.anyhit != VK_NULL_HANDLE) {
            anyhit_stage = add_stage(VK_SHADER_STAGE_ANY_HIT_BIT_KHR, desc.anyhit);
        }

        // --- Shader groups ---
        // Group 0: raygen
        // Group 1: environment miss
        // Group 2: shadow miss
        // Group 3: hit group (closesthit + optional anyhit)

        std::array<VkRayTracingShaderGroupCreateInfoKHR, 4> groups{};
        for (auto &g: groups) {
            g.sType = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR;
            g.generalShader = VK_SHADER_UNUSED_KHR;
            g.closestHitShader = VK_SHADER_UNUSED_KHR;
            g.anyHitShader = VK_SHADER_UNUSED_KHR;
            g.intersectionShader = VK_SHADER_UNUSED_KHR;
        }

        // Raygen group
        groups[0].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
        groups[0].generalShader = raygen_stage;

        // Environment miss group
        groups[1].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
        groups[1].generalShader = miss_stage;

        // Shadow miss group
        groups[2].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
        groups[2].generalShader = shadow_miss_stage;

        // Hit group (triangles, closesthit + optional anyhit)
        groups[3].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR;
        groups[3].closestHitShader = closesthit_stage;
        groups[3].anyHitShader = anyhit_stage;

        // --- Pipeline layout ---

        VkPipelineLayoutCreateInfo layout_info{};
        layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        layout_info.setLayoutCount = static_cast<uint32_t>(desc.descriptor_set_layouts.size());
        layout_info.pSetLayouts = desc.descriptor_set_layouts.data();
        layout_info.pushConstantRangeCount = static_cast<uint32_t>(desc.push_constant_ranges.size());
        layout_info.pPushConstantRanges = desc.push_constant_ranges.data();

        VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
        VK_CHECK(vkCreatePipelineLayout(device, &layout_info, nullptr, &pipeline_layout));

        // --- RT pipeline creation ---

        VkRayTracingPipelineCreateInfoKHR pipeline_info{};
        pipeline_info.sType = VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CREATE_INFO_KHR;
        pipeline_info.stageCount = static_cast<uint32_t>(stages.size());
        pipeline_info.pStages = stages.data();
        pipeline_info.groupCount = static_cast<uint32_t>(groups.size());
        pipeline_info.pGroups = groups.data();
        pipeline_info.maxPipelineRayRecursionDepth = desc.max_recursion_depth;
        pipeline_info.layout = pipeline_layout;

        VkPipeline vk_pipeline = VK_NULL_HANDLE;
        VK_CHECK(ctx.pfn_create_rt_pipelines(device,
            VK_NULL_HANDLE,
            VK_NULL_HANDLE,
            1,
            &pipeline_info,
            nullptr,
            &vk_pipeline));

        // --- SBT construction ---

        const uint32_t handle_size = ctx.rt_shader_group_handle_size;
        const uint32_t handle_alignment = ctx.rt_shader_group_handle_alignment;
        const uint32_t base_alignment = ctx.rt_shader_group_base_alignment;
        constexpr auto group_count = static_cast<uint32_t>(groups.size());

        // Each SBT entry is handle_size aligned up to handle_alignment
        const uint32_t handle_size_aligned = align_up(handle_size, handle_alignment);

        // Region sizes (aligned to base_alignment)
        const uint32_t raygen_region_size = align_up(handle_size_aligned, base_alignment); // 1 entry
        const uint32_t miss_region_size = align_up(handle_size_aligned * 2, base_alignment); // 2 entries
        const uint32_t hit_region_size = align_up(handle_size_aligned, base_alignment); // 1 entry
        const uint32_t sbt_size = raygen_region_size + miss_region_size + hit_region_size;

        // Query all shader group handles
        std::vector<uint8_t> handles(handle_size * group_count);
        VK_CHECK(ctx.pfn_get_rt_shader_group_handles(
            device, vk_pipeline, 0, group_count, handles.size(), handles.data()));

        // Allocate SBT buffer (host-visible for direct write, needs device address)
        VkBufferCreateInfo sbt_buf_info{};
        sbt_buf_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        sbt_buf_info.size = sbt_size;
        sbt_buf_info.usage = VK_BUFFER_USAGE_SHADER_BINDING_TABLE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;

        VmaAllocationCreateInfo sbt_alloc_info{};
        sbt_alloc_info.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
        sbt_alloc_info.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;

        VkBuffer sbt_buffer = VK_NULL_HANDLE;
        VmaAllocation sbt_allocation = VK_NULL_HANDLE;
        VmaAllocationInfo sbt_alloc_result{};
        VK_CHECK(vmaCreateBuffer(allocator,
            &sbt_buf_info,
            &sbt_alloc_info,
            &sbt_buffer,
            &sbt_allocation,
            &sbt_alloc_result));

        // Write handles into SBT buffer regions
        auto *sbt_data = static_cast<uint8_t *>(sbt_alloc_result.pMappedData);
        std::memset(sbt_data, 0, sbt_size);

        // Group 0 → raygen region
        std::memcpy(sbt_data, handles.data() + 0 * handle_size, handle_size);

        // Group 1,2 → miss region
        uint8_t *miss_base = sbt_data + raygen_region_size;
        std::memcpy(miss_base, handles.data() + 1 * handle_size, handle_size);
        std::memcpy(miss_base + handle_size_aligned, handles.data() + 2 * handle_size, handle_size);

        // Group 3 → hit region
        uint8_t *hit_base = miss_base + miss_region_size;
        std::memcpy(hit_base, handles.data() + 3 * handle_size, handle_size);

        // Compute SBT buffer device address
        VkBufferDeviceAddressInfo addr_info{};
        addr_info.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
        addr_info.buffer = sbt_buffer;
        const VkDeviceAddress sbt_address = vkGetBufferDeviceAddress(device, &addr_info);

        // --- Populate result ---

        RTPipeline result{};
        result.pipeline = vk_pipeline;
        result.layout = pipeline_layout;
        result.sbt_buffer = sbt_buffer;
        result.sbt_allocation = sbt_allocation;

        result.raygen_region.deviceAddress = sbt_address;
        result.raygen_region.stride = raygen_region_size;
        result.raygen_region.size = raygen_region_size;

        result.miss_region.deviceAddress = sbt_address + raygen_region_size;
        result.miss_region.stride = handle_size_aligned;
        result.miss_region.size = miss_region_size;

        result.hit_region.deviceAddress = sbt_address + raygen_region_size + miss_region_size;
        result.hit_region.stride = handle_size_aligned;
        result.hit_region.size = hit_region_size;

        // callable_region stays zeroed (not used)

        spdlog::info("RT pipeline created: {} stages, {} groups, SBT {} bytes",
                     stages.size(),
                     group_count, sbt_size);

        return result;
    }

    void RTPipeline::destroy(VkDevice device, VmaAllocator allocator) {
        if (sbt_buffer != VK_NULL_HANDLE) {
            vmaDestroyBuffer(allocator, sbt_buffer, sbt_allocation);
            sbt_buffer = VK_NULL_HANDLE;
            sbt_allocation = VK_NULL_HANDLE;
        }
        if (pipeline != VK_NULL_HANDLE) {
            vkDestroyPipeline(device, pipeline, nullptr);
            pipeline = VK_NULL_HANDLE;
        }
        if (layout != VK_NULL_HANDLE) {
            vkDestroyPipelineLayout(device, layout, nullptr);
            layout = VK_NULL_HANDLE;
        }
    }
} // namespace himalaya::rhi
