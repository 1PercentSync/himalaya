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

    /** @brief Maximum number of textures in the bindless array (Set 1, Binding 0). */
    constexpr uint32_t kMaxBindlessTextures = 4096;

    /**
     * @brief Manages descriptor set layouts, descriptor pools, and bindless texture registration.
     *
     * Owns two descriptor set layouts:
     * - Set 0: per-frame global data (GlobalUBO + LightBuffer + MaterialBuffer)
     * - Set 1: bindless texture array (variable descriptor count, update-after-bind)
     *
     * Owns two descriptor pools:
     * - Normal pool for Set 0 (2 sets for 2 frames in flight)
     * - UPDATE_AFTER_BIND pool for Set 1 (1 set, shared across frames)
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
         * @return Array of {set0_layout, set1_layout}.
         */
        [[nodiscard]] std::array<VkDescriptorSetLayout, 2> get_global_set_layouts() const;

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
         * @brief Writes a buffer descriptor to a Set 0 binding across all frames in flight.
         *
         * Resolves the BufferHandle internally via ResourceManager.
         * Descriptor type is inferred from the binding index (0 = UBO, 1/2 = SSBO).
         * Writes to both per-frame Set 0 instances.
         *
         * @param binding Binding index within Set 0 (0 = GlobalUBO, 1 = LightBuffer, 2 = MaterialBuffer).
         * @param buffer  Buffer handle to bind.
         * @param range   Byte range of the buffer to expose to the shader.
         */
        void write_set0_buffer(uint32_t binding, BufferHandle buffer, uint64_t range) const;

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

    private:
        /** @brief Vulkan context (device). */
        Context *context_ = nullptr;

        /** @brief Resource manager for image/sampler lookups. */
        ResourceManager *resource_manager_ = nullptr;

        // ---- Layouts ----

        /** @brief Set 0: GlobalUBO (binding 0) + LightBuffer (binding 1) + MaterialBuffer (binding 2). */
        VkDescriptorSetLayout set0_layout_ = VK_NULL_HANDLE;

        /** @brief Set 1: bindless sampler2D array (binding 0). */
        VkDescriptorSetLayout set1_layout_ = VK_NULL_HANDLE;

        // ---- Pools ----

        /** @brief Normal descriptor pool for Set 0 allocation. */
        VkDescriptorPool set0_pool_ = VK_NULL_HANDLE;

        /** @brief UPDATE_AFTER_BIND descriptor pool for Set 1 allocation. */
        VkDescriptorPool set1_pool_ = VK_NULL_HANDLE;

        // ---- Allocated Sets ----

        /** @brief Per-frame Set 0 descriptor sets (one per frame in flight). */
        std::array<VkDescriptorSet, 2> set0_sets_{};

        /** @brief Single Set 1 descriptor set (bindless textures). */
        VkDescriptorSet set1_set_ = VK_NULL_HANDLE;

        // ---- Bindless free list ----

        /** @brief Next sequential bindless index for fresh allocation. */
        uint32_t next_bindless_index_ = 0;

        /** @brief Freed bindless indices available for reuse. */
        std::vector<uint32_t> free_bindless_indices_;

        // ---- Private helpers ----

        /** @brief Creates Set 0 and Set 1 descriptor set layouts. */
        void create_layouts();

        /** @brief Creates the two descriptor pools (normal + update-after-bind). */
        void create_pools();

        /** @brief Allocates Set 0 x2 and Set 1 x1 from their respective pools. */
        void allocate_sets();
    };
} // namespace himalaya::rhi
