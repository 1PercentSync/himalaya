#pragma once

/**
 * @file commands.h
 * @brief Command buffer wrapper for Vulkan command recording.
 */

#include <vulkan/vulkan.h>

namespace himalaya::rhi {

    /**
     * @brief Thin wrapper around VkCommandBuffer for convenient command recording.
     *
     * Does not own the underlying VkCommandBuffer — the caller (FrameData)
     * manages its lifetime. Upper layers interact exclusively through wrapper
     * methods and never touch VkCommandBuffer directly.
     */
    class CommandBuffer {
    public:
        /**
         * @brief Constructs a wrapper around an existing VkCommandBuffer.
         * @param cmd Raw Vulkan command buffer (must be valid for the wrapper's lifetime).
         */
        explicit CommandBuffer(VkCommandBuffer cmd);

    private:
        /** @brief Wrapped Vulkan command buffer. */
        VkCommandBuffer cmd_;
    };

} // namespace himalaya::rhi
