#include <himalaya/rhi/commands.h>
#include <himalaya/rhi/context.h>
#include <himalaya/rhi/pipeline.h>

namespace himalaya::rhi {
    // ReSharper disable once CppParameterMayBeConst
    CommandBuffer::CommandBuffer(VkCommandBuffer cmd) : cmd_(cmd) {
    }

    // Combines reset + begin since every frame starts fresh.
    void CommandBuffer::begin() const {
        VK_CHECK(vkResetCommandBuffer(cmd_, 0));

        VkCommandBufferBeginInfo begin_info{};
        begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        VK_CHECK(vkBeginCommandBuffer(cmd_, &begin_info));
    }

    void CommandBuffer::end() const {
        VK_CHECK(vkEndCommandBuffer(cmd_));
    }

    void CommandBuffer::begin_rendering(const VkRenderingInfo &rendering_info) const {
        vkCmdBeginRendering(cmd_, &rendering_info);
    }

    void CommandBuffer::end_rendering() const {
        vkCmdEndRendering(cmd_);
    }

    void CommandBuffer::bind_pipeline(const Pipeline &pipeline) const {
        vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.pipeline);
    }

    void CommandBuffer::bind_vertex_buffer(const uint32_t binding,
                                           // ReSharper disable once CppParameterMayBeConst
                                           VkBuffer buffer,
                                           const VkDeviceSize offset) const {
        vkCmdBindVertexBuffers(cmd_, binding, 1, &buffer, &offset);
    }

    // ReSharper disable once CppParameterMayBeConst
    void CommandBuffer::bind_index_buffer(VkBuffer buffer,
                                          const VkIndexType index_type,
                                          const VkDeviceSize offset) const {
        vkCmdBindIndexBuffer(cmd_, buffer, offset, index_type);
    }

    void CommandBuffer::draw(const uint32_t vertex_count,
                             const uint32_t instance_count,
                             const uint32_t first_vertex,
                             const uint32_t first_instance) const {
        vkCmdDraw(cmd_,
                  vertex_count,
                  instance_count,
                  first_vertex,
                  first_instance);
    }

    void CommandBuffer::draw_indexed(const uint32_t index_count,
                                     const uint32_t instance_count,
                                     const uint32_t first_index,
                                     const int32_t vertex_offset,
                                     const uint32_t first_instance) const {
        vkCmdDrawIndexed(cmd_,
                         index_count,
                         instance_count,
                         first_index,
                         vertex_offset,
                         first_instance);
    }

    void CommandBuffer::set_viewport(const VkViewport &viewport) const {
        vkCmdSetViewport(cmd_, 0, 1, &viewport);
    }

    void CommandBuffer::set_scissor(const VkRect2D &scissor) const {
        vkCmdSetScissor(cmd_, 0, 1, &scissor);
    }

    void CommandBuffer::pipeline_barrier(const VkDependencyInfo &dependency_info) const {
        vkCmdPipelineBarrier2(cmd_, &dependency_info);
    }

    // ReSharper disable once CppParameterMayBeConst
    void CommandBuffer::push_constants(VkPipelineLayout layout,
                                       const VkShaderStageFlags stages,
                                       const void *data,
                                       const uint32_t size,
                                       const uint32_t offset) const {
        vkCmdPushConstants(cmd_, layout, stages, offset, size, data);
    }

    // ReSharper disable once CppParameterMayBeConst
    void CommandBuffer::bind_descriptor_sets(VkPipelineLayout layout,
                                             const uint32_t first_set,
                                             const VkDescriptorSet *sets,
                                             const uint32_t count) const {
        vkCmdBindDescriptorSets(cmd_,
                                VK_PIPELINE_BIND_POINT_GRAPHICS,
                                layout,
                                first_set,
                                count,
                                sets,
                                0,
                                nullptr);
    }

    void CommandBuffer::bind_compute_pipeline(const Pipeline &pipeline) const {
        vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline.pipeline);
    }

    void CommandBuffer::dispatch(const uint32_t group_count_x,
                                 const uint32_t group_count_y,
                                 const uint32_t group_count_z) const {
        vkCmdDispatch(cmd_, group_count_x, group_count_y, group_count_z);
    }

    // ReSharper disable once CppParameterMayBeConst
    void CommandBuffer::push_descriptor_set(VkPipelineLayout layout,
                                            const uint32_t set,
                                            const std::span<const VkWriteDescriptorSet> writes) const {
        vkCmdPushDescriptorSet(cmd_,
                               VK_PIPELINE_BIND_POINT_COMPUTE,
                               layout,
                               set,
                               static_cast<uint32_t>(writes.size()),
                               writes.data());
    }

    void CommandBuffer::copy_buffer_to_image(const VkCopyBufferToImageInfo2 &copy_info) const {
        vkCmdCopyBufferToImage2(cmd_, &copy_info);
    }

    namespace {
        PFN_vkCmdBeginDebugUtilsLabelEXT pfn_begin_label = nullptr;
        PFN_vkCmdEndDebugUtilsLabelEXT pfn_end_label = nullptr;
    }

    // Loads extension function pointers via vkGetInstanceProcAddr.
    // vulkan-1.lib does not export VK_EXT_debug_utils command-level functions.
    // ReSharper disable once CppParameterMayBeConst
    void CommandBuffer::init_debug_functions(VkInstance instance) {
#ifndef NDEBUG
        pfn_begin_label = reinterpret_cast<PFN_vkCmdBeginDebugUtilsLabelEXT>(
            vkGetInstanceProcAddr(instance, "vkCmdBeginDebugUtilsLabelEXT"));
        pfn_end_label = reinterpret_cast<PFN_vkCmdEndDebugUtilsLabelEXT>(
            vkGetInstanceProcAddr(instance, "vkCmdEndDebugUtilsLabelEXT"));
#endif
    }

    void CommandBuffer::begin_debug_label(const char *name, const std::array<float, 4> color) const {
#ifndef NDEBUG
        if (pfn_begin_label) {
            VkDebugUtilsLabelEXT label{};
            label.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT;
            label.pLabelName = name;
            std::ranges::copy(color, label.color);
            pfn_begin_label(cmd_, &label);
        }
#endif
    }

    void CommandBuffer::end_debug_label() const {
#ifndef NDEBUG
        if (pfn_end_label) {
            pfn_end_label(cmd_);
        }
#endif
    }

    void CommandBuffer::set_cull_mode(const VkCullModeFlags cull_mode) const {
        vkCmdSetCullMode(cmd_, cull_mode);
    }

    void CommandBuffer::set_front_face(const VkFrontFace front_face) const {
        vkCmdSetFrontFace(cmd_, front_face);
    }

    void CommandBuffer::set_depth_test_enable(const bool enable) const {
        vkCmdSetDepthTestEnable(cmd_, enable ? VK_TRUE : VK_FALSE);
    }

    void CommandBuffer::set_depth_write_enable(const bool enable) const {
        vkCmdSetDepthWriteEnable(cmd_, enable ? VK_TRUE : VK_FALSE);
    }

    void CommandBuffer::set_depth_compare_op(const VkCompareOp compare_op) const {
        vkCmdSetDepthCompareOp(cmd_, compare_op);
    }

    void CommandBuffer::set_depth_bias(const float constant_factor,
                                       const float clamp,
                                       const float slope_factor) const {
        vkCmdSetDepthBias(cmd_, constant_factor, clamp, slope_factor);
    }
} // namespace himalaya::rhi
