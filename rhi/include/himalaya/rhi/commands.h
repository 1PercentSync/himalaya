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

        /**
         * @brief Resets and begins recording with ONE_TIME_SUBMIT usage.
         *
         * Combines vkResetCommandBuffer + vkBeginCommandBuffer into one call
         * since they are always paired in our frame loop.
         */
        void begin() const;

        /**
         * @brief Ends command buffer recording.
         */
        void end() const;

        /**
         * @brief Begins a dynamic rendering pass.
         * @param rendering_info Rendering configuration (render area, attachments, etc.).
         */
        void begin_rendering(const VkRenderingInfo &rendering_info) const;

        /**
         * @brief Ends the current dynamic rendering pass.
         */
        void end_rendering() const;

    private:
        /** @brief Wrapped Vulkan command buffer. */
        VkCommandBuffer cmd_;
    };

} // namespace himalaya::rhi
