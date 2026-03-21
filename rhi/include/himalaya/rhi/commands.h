#pragma once

/**
 * @file commands.h
 * @brief Command buffer wrapper for Vulkan command recording.
 */

#include <himalaya/rhi/types.h>

#include <vulkan/vulkan.h>

#include <array>
#include <span>

namespace himalaya::rhi {
    class ResourceManager;
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
         * @brief Binds an index buffer for subsequent indexed draw calls.
         * @param buffer     Vulkan buffer containing index data.
         * @param index_type Index element type (UINT16 or UINT32).
         * @param offset     Byte offset into the buffer (default 0).
         */
        void bind_index_buffer(VkBuffer buffer,
                               VkIndexType index_type,
                               VkDeviceSize offset = 0) const;

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
         * @brief Records an indexed draw call.
         * @param index_count    Number of indices to draw.
         * @param instance_count Number of instances (default 1).
         * @param first_index    Starting index in the index buffer (default 0).
         * @param vertex_offset  Value added to each index before fetching vertices (default 0).
         * @param first_instance First instance ID (default 0).
         */
        void draw_indexed(uint32_t index_count,
                          uint32_t instance_count = 1,
                          uint32_t first_index = 0,
                          int32_t vertex_offset = 0,
                          uint32_t first_instance = 0) const;

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

        // --- Synchronization & Transfer ---

        /**
         * @brief Inserts a pipeline barrier using the Synchronization2 API.
         * @param dependency_info Barrier specification (image/buffer/memory barriers).
         */
        void pipeline_barrier(const VkDependencyInfo &dependency_info) const;

        /**
         * @brief Copies data from a buffer to an image.
         *
         * Thin wrapper around vkCmdCopyBufferToImage2. Used for uploading
         * block-compressed data (e.g. BC6H) from a staging buffer to a GPU image.
         *
         * @param copy_info Fully populated VkCopyBufferToImageInfo2.
         */
        void copy_buffer_to_image(const VkCopyBufferToImageInfo2 &copy_info) const;

        // --- Push constants ---

        /**
         * @brief Updates push constant data for the bound pipeline.
         * @param layout Pipeline layout defining the push constant ranges.
         * @param stages Shader stages that will access the push constants.
         * @param data   Pointer to the push constant data.
         * @param size   Size of the push constant data in bytes.
         * @param offset Byte offset into the push constant block (default 0).
         */
        void push_constants(VkPipelineLayout layout,
                            VkShaderStageFlags stages,
                            const void *data, uint32_t size,
                            uint32_t offset = 0) const;

        // --- Descriptor binding ---

        /**
         * @brief Binds descriptor sets to the graphics pipeline.
         * @param layout     Pipeline layout that the sets are compatible with.
         * @param first_set  Index of the first descriptor set to bind.
         * @param sets       Pointer to an array of descriptor sets.
         * @param count      Number of descriptor sets to bind.
         */
        void bind_descriptor_sets(VkPipelineLayout layout, uint32_t first_set,
                                  const VkDescriptorSet *sets, uint32_t count) const;

        /**
         * @brief Binds descriptor sets to the compute pipeline.
         * @param layout     Pipeline layout that the sets are compatible with.
         * @param first_set  Index of the first descriptor set to bind.
         * @param sets       Pointer to an array of descriptor sets.
         * @param count      Number of descriptor sets to bind.
         */
        void bind_compute_descriptor_sets(VkPipelineLayout layout, uint32_t first_set,
                                          const VkDescriptorSet *sets, uint32_t count) const;

        // --- Compute ---

        /**
         * @brief Binds a compute pipeline for subsequent dispatch commands.
         * @param pipeline Pipeline to bind (must have been created with create_compute_pipeline).
         */
        void bind_compute_pipeline(const Pipeline &pipeline) const;

        /**
         * @brief Dispatches compute work groups.
         * @param group_count_x Number of work groups in X dimension.
         * @param group_count_y Number of work groups in Y dimension.
         * @param group_count_z Number of work groups in Z dimension.
         */
        void dispatch(uint32_t group_count_x, uint32_t group_count_y, uint32_t group_count_z) const;

        /**
         * @brief Pushes descriptors directly without pre-allocated descriptor sets.
         *
         * Vulkan 1.4 core (promoted from VK_KHR_push_descriptor).
         * Used exclusively for IBL one-time init compute dispatches.
         *
         * @param layout Pipeline layout compatible with the pushed descriptors.
         * @param set    Descriptor set index to push to.
         * @param writes Descriptor write operations.
         */
        void push_descriptor_set(VkPipelineLayout layout, uint32_t set,
                                 std::span<const VkWriteDescriptorSet> writes) const;

        /**
         * @brief Pushes a storage image descriptor for compute output.
         *
         * Resolves ImageHandle to VkImageView via ResourceManager and pushes
         * a STORAGE_IMAGE descriptor with GENERAL layout.
         *
         * @param rm      Resource manager for handle resolution.
         * @param layout  Pipeline layout compatible with the push descriptor set.
         * @param set     Descriptor set index to push to.
         * @param binding Binding index within the set.
         * @param image   Image handle for the storage image.
         */
        void push_storage_image(const ResourceManager &rm, VkPipelineLayout layout,
                                uint32_t set, uint32_t binding, ImageHandle image) const;

        /**
         * @brief Pushes a sampled image descriptor for compute input.
         *
         * Resolves ImageHandle and SamplerHandle via ResourceManager and pushes
         * a COMBINED_IMAGE_SAMPLER descriptor with SHADER_READ_ONLY_OPTIMAL layout.
         *
         * @param rm      Resource manager for handle resolution.
         * @param layout  Pipeline layout compatible with the push descriptor set.
         * @param set     Descriptor set index to push to.
         * @param binding Binding index within the set.
         * @param image   Image handle for the sampled image.
         * @param sampler Sampler handle for sampling parameters.
         */
        void push_sampled_image(const ResourceManager &rm, VkPipelineLayout layout,
                                uint32_t set, uint32_t binding, ImageHandle image,
                                SamplerHandle sampler) const;

        // --- Debug labels (VK_EXT_debug_utils, debug builds only) ---

        /**
         * @brief Loads VK_EXT_debug_utils function pointers for debug labels.
         *
         * Must be called once after instance creation. No-op in release builds.
         *
         * @param instance Vulkan instance with VK_EXT_debug_utils enabled.
         */
        static void init_debug_functions(VkInstance instance);

        /**
         * @brief Begins a named debug label region for GPU profiler grouping.
         *
         * No-op in release builds (VK_EXT_debug_utils not enabled).
         *
         * @param name  Label name (visible in RenderDoc, Nsight, etc.).
         * @param color RGBA color for the label region.
         */
        void begin_debug_label(const char *name, std::array<float, 4> color) const;

        /**
         * @brief Ends the current debug label region.
         *
         * No-op in release builds.
         */
        void end_debug_label() const;

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

        /**
         * @brief Sets the dynamic depth bias parameters.
         *
         * Only takes effect on pipelines created with depth_bias_enable = true.
         *
         * @param constant_factor Constant depth value added to each fragment.
         * @param clamp           Maximum (or minimum) depth bias of a fragment (0 = no clamp).
         * @param slope_factor    Scalar factor applied to fragment's slope in depth bias calculation.
         */
        void set_depth_bias(float constant_factor, float clamp, float slope_factor) const;

        /** @brief Returns the underlying Vulkan command buffer handle. */
        [[nodiscard]] VkCommandBuffer handle() const { return cmd_; }

    private:
        /** @brief Wrapped Vulkan command buffer. */
        VkCommandBuffer cmd_;
    };
} // namespace himalaya::rhi
