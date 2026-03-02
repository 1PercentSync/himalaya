#include <himalaya/rhi/commands.h>
#include <himalaya/rhi/context.h>
#include <himalaya/rhi/pipeline.h>

namespace himalaya::rhi {
    CommandBuffer::CommandBuffer(const VkCommandBuffer cmd) : cmd_(cmd) {
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
} // namespace himalaya::rhi
