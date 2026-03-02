#include <himalaya/rhi/commands.h>
#include <himalaya/rhi/context.h>

namespace himalaya::rhi {

    CommandBuffer::CommandBuffer(const VkCommandBuffer cmd) : cmd_(cmd) {}

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

} // namespace himalaya::rhi
