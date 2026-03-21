#pragma once

/**
 * @file descriptors.h
 * @brief Descriptor management: set layouts, descriptor pools, and bindless texture array.
 */

#include <himalaya/rhi/types.h>

#include <array>
#include <vector>

#include <vulkan/vulkan.h>

namespace himalaya::rhi {
    class Context;
    class ResourceManager;

    /** @brief Maximum number of 2D textures in the bindless array (Set 1, Binding 0). */
    constexpr uint32_t kMaxBindlessTextures = 4096;

    /** @brief Maximum number of cubemaps in the bindless array (Set 1, Binding 1). */
    constexpr uint32_t kMaxBindlessCubemaps = 256;

    /** @brief Number of render target bindings in Set 2 (pre-allocated for all M1 phases). */
    constexpr uint32_t kRenderTargetBindingCount = 8;

    /**
     * @brief Manages descriptor set layouts, descriptor pools, and bindless texture registration.
     *
     * Owns three descriptor set layouts:
     * - Set 0: per-frame global data (GlobalUBO + LightBuffer + MaterialBuffer + InstanceBuffer)
     * - Set 1: bindless arrays (binding 0: sampler2D[], binding 1: samplerCube[])
     * - Set 2: render target intermediates (8 named bindings, PARTIALLY_BOUND)
     *
     * Owns three descriptor pools:
     * - Normal pool for Set 0 (2 sets for 2 frames in flight)
     * - UPDATE_AFTER_BIND pool for Set 1 (1 set, shared across frames)
     * - Normal pool for Set 2 (2 sets for 2 frames in flight, per-frame temporal updates)
     *
     * Lifetime is managed explicitly via init() and destroy().
     */
    class DescriptorManager {
    public:
        /**
         * @brief Initializes layouts, pools, and allocates descriptor sets.
         * @param context          Vulkan context providing device.
         * @param resource_manager Resource manager for texture/sampler lookups.
         */
        void init(Context *context, ResourceManager *resource_manager);

        /**
         * @brief Destroys all descriptor pools and layouts.
         *
         * Must be called before the Vulkan device is destroyed.
         */
        void destroy();

        /**
         * @brief Returns the global set layouts for pipeline creation.
         * @return Array of {set0_layout, set1_layout, set2_layout}.
         */
        [[nodiscard]] std::array<VkDescriptorSetLayout, 3> get_global_set_layouts() const;

        /**
         * @brief Returns the Set 0 descriptor set for the given frame index.
         * @param frame_index Frame in flight index (0 to kMaxFramesInFlight-1).
         * @return VkDescriptorSet for the requested frame.
         */
        [[nodiscard]] VkDescriptorSet get_set0(uint32_t frame_index) const;

        /**
         * @brief Returns the single Set 1 (bindless textures) descriptor set.
         * @return VkDescriptorSet for the bindless texture array.
         */
        [[nodiscard]] VkDescriptorSet get_set1() const;

        /**
         * @brief Returns the Set 2 (render targets) descriptor set for the given frame.
         * @param frame_index Frame in flight index (0 to kMaxFramesInFlight-1).
         * @return VkDescriptorSet for the render target bindings of the requested frame.
         */
        [[nodiscard]] VkDescriptorSet get_set2(uint32_t frame_index) const;

        /**
         * @brief Registers a texture+sampler pair into the bindless array.
         *
         * Writes a combined image sampler descriptor into the next available
         * slot in the bindless array. The returned index is used in shaders
         * to sample the texture.
         *
         * @param image   Image handle (must be valid).
         * @param sampler Sampler handle (must be valid).
         * @return Index into the bindless array for shader access.
         */
        [[nodiscard]] BindlessIndex register_texture(ImageHandle image, SamplerHandle sampler);

        /**
         * @brief Writes a buffer descriptor to a Set 0 binding for a specific frame.
         *
         * Use this for per-frame resources (GlobalUBO, LightBuffer) where each
         * frame in flight has its own buffer.
         *
         * @param frame_index Frame in flight index (0 to kMaxFramesInFlight-1).
         * @param binding     Binding index within Set 0 (0 = GlobalUBO, 1 = LightBuffer, 2 = MaterialBuffer, 3 = InstanceBuffer).
         * @param buffer      Buffer handle to bind.
         * @param range       Byte range of the buffer to expose to the shader.
         */
        void write_set0_buffer(uint32_t frame_index, uint32_t binding, BufferHandle buffer, uint64_t range) const;

        /**
         * @brief Writes a buffer descriptor to a Set 0 binding across all frames in flight.
         *
         * Use this for frame-invariant resources (MaterialBuffer) shared across all frames.
         *
         * @param binding Binding index within Set 0 (0 = GlobalUBO, 1 = LightBuffer, 2 = MaterialBuffer, 3 = InstanceBuffer).
         * @param buffer  Buffer handle to bind.
         * @param range   Byte range of the buffer to expose to the shader.
         */
        void write_set0_buffer(uint32_t binding, BufferHandle buffer, uint64_t range) const;

        /**
         * @brief Registers a cubemap+sampler pair into the bindless cubemap array.
         *
         * Writes a combined image sampler descriptor into Set 1, binding 1.
         * Uses an independent slot space and free list from 2D textures.
         *
         * @param image   Cubemap image handle (must be valid, VK_IMAGE_VIEW_TYPE_CUBE).
         * @param sampler Sampler handle (must be valid).
         * @return Index into the cubemaps[] array for shader access.
         */
        [[nodiscard]] BindlessIndex register_cubemap(ImageHandle image, SamplerHandle sampler);

        /**
         * @brief Unregisters a texture from the bindless array.
         *
         * The slot is returned to the free list for reuse.
         * The caller is responsible for ensuring the GPU is no longer
         * referencing this slot (e.g. via deferred deletion).
         *
         * @param index Bindless index to unregister.
         */
        void unregister_texture(BindlessIndex index);

        /**
         * @brief Unregisters a cubemap from the bindless cubemap array.
         *
         * The slot is returned to the cubemap free list for reuse.
         * The caller is responsible for ensuring the GPU is no longer
         * referencing this slot (e.g. via deferred deletion).
         *
         * @param index Bindless index to unregister.
         */
        void unregister_cubemap(BindlessIndex index);

        /**
         * @brief Updates a Set 2 render target binding across all frames in flight.
         *
         * Called at init, resize, or MSAA switch to point a named render target
         * binding to its current backing image. Writes to both per-frame copies.
         *
         * @param binding Binding index within Set 2 (0-7).
         * @param image   Image handle for the render target.
         * @param sampler Sampler handle for sampling the render target.
         */
        void update_render_target(uint32_t binding, ImageHandle image, SamplerHandle sampler) const;

        /**
         * @brief Updates a Set 2 render target binding for a specific frame.
         *
         * Used for temporal bindings that swap backing images each frame:
         * only the current frame's Set 2 copy is updated.
         *
         * @param frame_index Frame in flight index (0 to kMaxFramesInFlight-1).
         * @param binding     Binding index within Set 2 (0-7).
         * @param image       Image handle for the render target.
         * @param sampler     Sampler handle for sampling the render target.
         */
        void update_render_target(uint32_t frame_index, uint32_t binding,
                                  ImageHandle image, SamplerHandle sampler) const;

        /**
         * @brief Returns descriptor set layouts for compute pipeline creation.
         *
         * Combines the three global set layouts (Set 0-2) with a caller-provided
         * Set 3 push descriptor layout, avoiding manual layout assembly in each
         * compute pass.
         *
         * @param set3_push_layout Push descriptor set layout for Set 3 (per-pass I/O).
         * @return Vector of {set0, set1, set2, set3} layouts.
         */
        [[nodiscard]] std::vector<VkDescriptorSetLayout> get_compute_set_layouts(
            VkDescriptorSetLayout set3_push_layout) const;

    private:
        /** @brief Vulkan context (device). */
        Context *context_ = nullptr;

        /** @brief Resource manager for image/sampler lookups. */
        ResourceManager *resource_manager_ = nullptr;

        // ---- Layouts ----

        /** @brief Set 0: GlobalUBO (0) + LightBuffer (1) + MaterialBuffer (2) + InstanceBuffer (3). */
        VkDescriptorSetLayout set0_layout_ = VK_NULL_HANDLE;

        /** @brief Set 1: bindless sampler2D array (binding 0) + samplerCube array (binding 1). */
        VkDescriptorSetLayout set1_layout_ = VK_NULL_HANDLE;

        /** @brief Set 2: render target intermediates (8 named bindings, PARTIALLY_BOUND). */
        VkDescriptorSetLayout set2_layout_ = VK_NULL_HANDLE;

        // ---- Pools ----

        /** @brief Normal descriptor pool for Set 0 allocation. */
        VkDescriptorPool set0_pool_ = VK_NULL_HANDLE;

        /** @brief UPDATE_AFTER_BIND descriptor pool for Set 1 allocation. */
        VkDescriptorPool set1_pool_ = VK_NULL_HANDLE;

        /** @brief Normal descriptor pool for Set 2 allocation. */
        VkDescriptorPool set2_pool_ = VK_NULL_HANDLE;

        // ---- Allocated Sets ----

        /** @brief Per-frame Set 0 descriptor sets (one per frame in flight). */
        std::array<VkDescriptorSet, 2> set0_sets_{};

        /** @brief Single Set 1 descriptor set (bindless textures). */
        VkDescriptorSet set1_set_ = VK_NULL_HANDLE;

        /** @brief Per-frame Set 2 descriptor sets (one per frame in flight). */
        std::array<VkDescriptorSet, 2> set2_sets_{};

        // ---- Bindless free lists ----

        /** @brief Next sequential texture index for fresh allocation (binding 0). */
        uint32_t next_bindless_index_ = 0;

        /** @brief Freed texture indices available for reuse (binding 0). */
        std::vector<uint32_t> free_bindless_indices_;

        /** @brief Next sequential cubemap index for fresh allocation (binding 1). */
        uint32_t next_cubemap_index_ = 0;

        /** @brief Freed cubemap indices available for reuse (binding 1). */
        std::vector<uint32_t> free_cubemap_indices_;

        // ---- Private helpers ----

        /** @brief Creates Set 0 and Set 1 descriptor set layouts. */
        void create_layouts();

        /** @brief Creates the two descriptor pools (normal + update-after-bind). */
        void create_pools();

        /** @brief Allocates Set 0 x2 and Set 1 x1 from their respective pools. */
        void allocate_sets();
    };
} // namespace himalaya::rhi
