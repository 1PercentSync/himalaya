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
    void CommandBuffer::bind_descriptor_sets(VkPipelineLayout layout,
                                             const uint32_t first_set,
                                             const VkDescriptorSet *sets,
                                             const uint32_t count) const {
        vkCmdBindDescriptorSets(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                layout, first_set, count, sets, 0, nullptr);
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
} // namespace himalaya::rhi
