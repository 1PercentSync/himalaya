/**
 * @file compute_utils.cpp
 * @brief Shared Vulkan compute-pass helper implementation.
 */

#include <himalaya/rhi/compute_utils.h>

#include <himalaya/rhi/commands.h>
#include <himalaya/rhi/context.h>
#include <himalaya/rhi/descriptors.h>
#include <himalaya/rhi/resources.h>
#include <himalaya/rhi/shader.h>

#include <array>
#include <vector>

namespace himalaya::rhi {
    VkDescriptorSetLayoutBinding storage_buffer_binding(const uint32_t binding,
                                                        const uint32_t descriptor_count,
                                                        const VkShaderStageFlags stage_flags) {
        return VkDescriptorSetLayoutBinding{
            .binding = binding,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .descriptorCount = descriptor_count,
            .stageFlags = stage_flags,
        };
    }

    VkDescriptorSetLayout create_push_storage_descriptor_set_layout(
        Context &ctx,
        const std::span<const VkDescriptorSetLayoutBinding> bindings) {
        VkDescriptorSetLayoutCreateInfo layout_ci{};
        layout_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        layout_ci.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT;
        layout_ci.bindingCount = static_cast<uint32_t>(bindings.size());
        layout_ci.pBindings = bindings.data();

        VkDescriptorSetLayout layout = VK_NULL_HANDLE;
        VK_CHECK(vkCreateDescriptorSetLayout(ctx.device, &layout_ci, nullptr, &layout));
        return layout;
    }

    VkDescriptorBufferInfo storage_buffer_info(const ResourceManager &rm, const BufferHandle handle) {
        const auto &buffer = rm.get_buffer(handle);
        return VkDescriptorBufferInfo{
            .buffer = buffer.buffer,
            .offset = 0,
            .range = buffer.desc.size,
        };
    }

    Pipeline create_compute_pipeline_from_file(Context &ctx,
                                               DescriptorManager &dm,
                                               ShaderCompiler &sc,
                                               const char *shader_path,
                                               const VkDescriptorSetLayout set3_layout,
                                               const std::span<const VkPushConstantRange> push_ranges) {
        const auto spirv = sc.compile_from_file(shader_path, ShaderStage::Compute);
        if (spirv.empty()) {
            return {};
        }

        const auto shader_module = create_shader_module(ctx.device, spirv);
        const auto set_layouts = dm.get_dispatch_set_layouts(set3_layout);
        const ComputePipelineDesc desc{
            .compute_shader = shader_module,
            .descriptor_set_layouts = set_layouts,
            .push_constant_ranges = {push_ranges.begin(), push_ranges.end()},
        };
        auto pipeline = create_compute_pipeline(ctx.device, desc);
        vkDestroyShaderModule(ctx.device, shader_module, nullptr);

        return pipeline;
    }

    void bind_dispatch_descriptor_sets(const CommandBuffer &cmd,
                                       const DescriptorManager &dm,
                                       const Pipeline &pipeline,
                                       const uint32_t frame_index) {
        const std::array sets = {
            dm.get_set0(frame_index),
            dm.get_set1(),
            dm.get_set2(frame_index),
        };
        cmd.bind_compute_descriptor_sets(pipeline.layout, 0, sets.data(), static_cast<uint32_t>(sets.size()));
    }

    void push_storage_buffers(const CommandBuffer &cmd,
                              const Pipeline &pipeline,
                              const std::span<const VkDescriptorBufferInfo> infos,
                              const uint32_t set) {
        std::vector<VkWriteDescriptorSet> writes(infos.size());
        for (uint32_t i = 0; i < infos.size(); ++i) {
            writes[i] = VkWriteDescriptorSet{
                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .dstBinding = i,
                .descriptorCount = 1,
                .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                .pBufferInfo = &infos[i],
            };
        }

        cmd.push_compute_descriptor_set(
            pipeline.layout,
            set,
            std::span<const VkWriteDescriptorSet>(writes.data(), writes.size()));
    }

    void buffer_barrier(const CommandBuffer &cmd,
                        const ResourceManager &rm,
                        const BufferHandle buffer,
                        const VkPipelineStageFlags2 src_stage,
                        const VkAccessFlags2 src_access,
                        const VkPipelineStageFlags2 dst_stage,
                        const VkAccessFlags2 dst_access,
                        const VkDeviceSize offset,
                        const VkDeviceSize size) {
        if (!buffer.valid()) {
            return;
        }

        const auto &resolved = rm.get_buffer(buffer);
        VkBufferMemoryBarrier2 barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
        barrier.srcStageMask = src_stage;
        barrier.srcAccessMask = src_access;
        barrier.dstStageMask = dst_stage;
        barrier.dstAccessMask = dst_access;
        barrier.buffer = resolved.buffer;
        barrier.offset = offset;
        barrier.size = (size == VK_WHOLE_SIZE) ? resolved.desc.size : size;

        VkDependencyInfo dep{};
        dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dep.bufferMemoryBarrierCount = 1;
        dep.pBufferMemoryBarriers = &barrier;
        cmd.pipeline_barrier(dep);
    }

    void buffer_barriers(const CommandBuffer &cmd,
                         const ResourceManager &rm,
                         const std::span<const BufferHandle> buffers,
                         const VkPipelineStageFlags2 src_stage,
                         const VkAccessFlags2 src_access,
                         const VkPipelineStageFlags2 dst_stage,
                         const VkAccessFlags2 dst_access) {
        std::vector<VkBufferMemoryBarrier2> barriers;
        barriers.reserve(buffers.size());

        for (const auto buffer : buffers) {
            if (!buffer.valid()) {
                continue;
            }

            const auto &resolved = rm.get_buffer(buffer);
            VkBufferMemoryBarrier2 barrier{};
            barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
            barrier.srcStageMask = src_stage;
            barrier.srcAccessMask = src_access;
            barrier.dstStageMask = dst_stage;
            barrier.dstAccessMask = dst_access;
            barrier.buffer = resolved.buffer;
            barrier.offset = 0;
            barrier.size = resolved.desc.size;
            barriers.push_back(barrier);
        }

        if (barriers.empty()) {
            return;
        }

        VkDependencyInfo dep{};
        dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dep.bufferMemoryBarrierCount = static_cast<uint32_t>(barriers.size());
        dep.pBufferMemoryBarriers = barriers.data();
        cmd.pipeline_barrier(dep);
    }
} // namespace himalaya::rhi
