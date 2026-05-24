#pragma once

/**
 * @file compute_utils.h
 * @brief Shared Vulkan compute-pass helper functions.
 */

#include <himalaya/rhi/pipeline.h>
#include <himalaya/rhi/types.h>

#include <vulkan/vulkan.h>

#include <cstdint>
#include <span>

namespace himalaya::rhi {
    class CommandBuffer;
    class Context;
    class DescriptorManager;
    class ResourceManager;
    class ShaderCompiler;

    /**
     * @brief Creates a storage-buffer descriptor layout binding for compute shaders.
     * @param binding Descriptor binding index.
     * @param descriptor_count Number of descriptors in the binding.
     * @param stage_flags Shader stages that access the binding.
     * @return Fully populated descriptor set layout binding.
     */
    [[nodiscard]] VkDescriptorSetLayoutBinding storage_buffer_binding(
        uint32_t binding,
        uint32_t descriptor_count = 1,
        VkShaderStageFlags stage_flags = VK_SHADER_STAGE_COMPUTE_BIT);

    /**
     * @brief Creates a Set 3 push-descriptor layout from storage-buffer bindings.
     * @param ctx Vulkan context owning the logical device.
     * @param bindings Descriptor bindings to include in the layout.
     * @return Created descriptor set layout handle.
     */
    [[nodiscard]] VkDescriptorSetLayout create_push_storage_descriptor_set_layout(
        Context &ctx,
        std::span<const VkDescriptorSetLayoutBinding> bindings);

    /**
     * @brief Resolves a BufferHandle into a full-range VkDescriptorBufferInfo.
     * @param rm Resource manager that owns the buffer.
     * @param handle Buffer handle to resolve.
     * @return Descriptor buffer info covering the whole buffer.
     */
    [[nodiscard]] VkDescriptorBufferInfo storage_buffer_info(const ResourceManager &rm,
                                                            BufferHandle handle);

    /**
     * @brief Creates a compute pipeline from a shader file using the standard dispatch layouts.
     *
     * The pipeline layout is built from DescriptorManager's dispatch Set 0/1/2
     * layouts plus the provided Set 3 push-descriptor layout.
     *
     * @param ctx Vulkan context owning the logical device.
     * @param dm Descriptor manager providing dispatch descriptor set layouts.
     * @param sc Shader compiler used to compile the compute shader.
     * @param shader_path Path relative to the shader include/root directory.
     * @param set3_layout Push-descriptor layout used as Set 3.
     * @param push_ranges Push constant ranges for the pipeline layout.
     * @return Created compute pipeline, or an empty Pipeline when shader compilation fails.
     */
    [[nodiscard]] Pipeline create_compute_pipeline_from_file(
        Context &ctx,
        DescriptorManager &dm,
        ShaderCompiler &sc,
        const char *shader_path,
        VkDescriptorSetLayout set3_layout,
        std::span<const VkPushConstantRange> push_ranges);

    /**
     * @brief Binds the standard dispatch descriptor sets 0-2 for a compute pipeline.
     * @param cmd Command buffer to record into.
     * @param dm Descriptor manager providing Set 0/1/2.
     * @param pipeline Pipeline whose layout is compatible with the dispatch sets.
     * @param frame_index Current frame-in-flight index for per-frame descriptor sets.
     */
    void bind_dispatch_descriptor_sets(const CommandBuffer &cmd,
                                       const DescriptorManager &dm,
                                       const Pipeline &pipeline,
                                       uint32_t frame_index);

    /**
     * @brief Pushes sequential storage-buffer descriptors to a compute Set 3 layout.
     *
     * Descriptor bindings are assigned sequentially starting from binding 0.
     *
     * @param cmd Command buffer to record into.
     * @param pipeline Pipeline whose layout contains the target push descriptor set.
     * @param infos Buffer descriptor infos to push.
     * @param set Descriptor set index to push, normally 3.
     */
    void push_storage_buffers(const CommandBuffer &cmd,
                              const Pipeline &pipeline,
                              std::span<const VkDescriptorBufferInfo> infos,
                              uint32_t set = 3);

    /**
     * @brief Inserts a whole-buffer or subrange buffer memory barrier.
     *
     * Invalid handles are ignored so callers may pass optional buffers directly.
     *
     * @param cmd Command buffer to record into.
     * @param rm Resource manager used to resolve the buffer handle.
     * @param buffer Buffer to synchronize.
     * @param src_stage Source pipeline stage mask.
     * @param src_access Source access mask.
     * @param dst_stage Destination pipeline stage mask.
     * @param dst_access Destination access mask.
     * @param offset Byte offset into the buffer.
     * @param size Byte size to synchronize, or VK_WHOLE_SIZE for the full buffer.
     */
    void buffer_barrier(const CommandBuffer &cmd,
                        const ResourceManager &rm,
                        BufferHandle buffer,
                        VkPipelineStageFlags2 src_stage,
                        VkAccessFlags2 src_access,
                        VkPipelineStageFlags2 dst_stage,
                        VkAccessFlags2 dst_access,
                        VkDeviceSize offset = 0,
                        VkDeviceSize size = VK_WHOLE_SIZE);

    /**
     * @brief Inserts identical barriers for multiple buffers in one vkCmdPipelineBarrier2 call.
     *
     * Invalid handles are skipped. If no valid buffer remains, no command is recorded.
     *
     * @param cmd Command buffer to record into.
     * @param rm Resource manager used to resolve buffer handles.
     * @param buffers Buffers to synchronize.
     * @param src_stage Source pipeline stage mask.
     * @param src_access Source access mask.
     * @param dst_stage Destination pipeline stage mask.
     * @param dst_access Destination access mask.
     */
    void buffer_barriers(const CommandBuffer &cmd,
                         const ResourceManager &rm,
                         std::span<const BufferHandle> buffers,
                         VkPipelineStageFlags2 src_stage,
                         VkAccessFlags2 src_access,
                         VkPipelineStageFlags2 dst_stage,
                         VkAccessFlags2 dst_access);
} // namespace himalaya::rhi
