#pragma once

/**
 * @file commands.h
 * @brief Command buffer wrapper for Vulkan command recording.
 */

#include <vulkan/vulkan.h>

namespace himalaya::rhi {

    struct Pipeline;

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

        /**
         * @brief Binds a graphics pipeline for subsequent draw commands.
         * @param pipeline Pipeline to bind (both layout and pipeline handle are used).
         */
        void bind_pipeline(const Pipeline &pipeline) const;

        /**
         * @brief Binds a vertex buffer to the given binding point.
         * @param binding Binding index (matches VkVertexInputBindingDescription::binding).
         * @param buffer  Vulkan buffer handle.
         * @param offset  Byte offset into the buffer.
         */
        void bind_vertex_buffer(uint32_t binding, VkBuffer buffer, VkDeviceSize offset = 0) const;

        /**
         * @brief Records a non-indexed draw call.
         * @param vertex_count   Number of vertices to draw.
         * @param instance_count Number of instances (default 1).
         * @param first_vertex   Offset into the vertex buffer (default 0).
         * @param first_instance First instance ID (default 0).
         */
        void draw(uint32_t vertex_count, uint32_t instance_count = 1,
                  uint32_t first_vertex = 0, uint32_t first_instance = 0) const;

        /**
         * @brief Sets the dynamic viewport state.
         * @param viewport Viewport dimensions and depth range.
         */
        void set_viewport(const VkViewport &viewport) const;

        /**
         * @brief Sets the dynamic scissor rectangle.
         * @param scissor Scissor region.
         */
        void set_scissor(const VkRect2D &scissor) const;

        /**
         * @brief Inserts a pipeline barrier using the Synchronization2 API.
         * @param dependency_info Barrier specification (image/buffer/memory barriers).
         */
        void pipeline_barrier(const VkDependencyInfo &dependency_info) const;

        // --- Extended Dynamic State ---

        /** @brief Sets the dynamic cull mode. */
        void set_cull_mode(VkCullModeFlags cull_mode) const;

        /** @brief Sets the dynamic front face winding order. */
        void set_front_face(VkFrontFace front_face) const;

        /** @brief Enables or disables depth testing. */
        void set_depth_test_enable(bool enable) const;

        /** @brief Enables or disables depth buffer writes. */
        void set_depth_write_enable(bool enable) const;

        /** @brief Sets the depth comparison operator. */
        void set_depth_compare_op(VkCompareOp compare_op) const;

    private:
        /** @brief Wrapped Vulkan command buffer. */
        VkCommandBuffer cmd_;
    };

} // namespace himalaya::rhi
