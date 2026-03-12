#pragma once

/**
 * @file types.h
 * @brief RHI type definitions: resource handles, shader stages, and format enums.
 */

#include <cstdint>

#include <vulkan/vulkan.h>

namespace himalaya::rhi {

    // ---- Resource Handles (generation-based) ----

    /**
     * @brief Image resource handle.
     *
     * Lightweight value type holding a slot index and generation counter.
     * The generation detects use-after-free: when a resource is destroyed,
     * the slot's generation increments, invalidating all existing handles.
     */
    struct ImageHandle {
        /** @brief Slot index in the resource pool (UINT32_MAX = invalid). */
        uint32_t index = UINT32_MAX;

        /** @brief Generation counter for use-after-free detection. */
        uint32_t generation = 0;

        /** @brief Returns true if the handle has been assigned a valid index. */
        [[nodiscard]] bool valid() const { return index != UINT32_MAX; }

        bool operator==(const ImageHandle &) const = default;
    };

    /**
     * @brief Buffer resource handle.
     * @see ImageHandle for generation-based semantics.
     */
    struct BufferHandle {
        uint32_t index = UINT32_MAX;
        uint32_t generation = 0;

        [[nodiscard]] bool valid() const { return index != UINT32_MAX; }
        bool operator==(const BufferHandle &) const = default;
    };

    /**
     * @brief Sampler resource handle.
     * @see ImageHandle for generation-based semantics.
     */
    struct SamplerHandle {
        uint32_t index = UINT32_MAX;
        uint32_t generation = 0;

        [[nodiscard]] bool valid() const { return index != UINT32_MAX; }
        bool operator==(const SamplerHandle &) const = default;
    };

    /**
     * @brief Index into the bindless texture array (Set 1, Binding 0).
     *
     * Shaders use this index to sample textures from the global descriptor array.
     */
    struct BindlessIndex {
        uint32_t index = UINT32_MAX;

        [[nodiscard]] bool valid() const { return index != UINT32_MAX; }
        bool operator==(const BindlessIndex &) const = default;
    };

    // ---- Enums ----

    /** @brief Shader compilation stage. */
    enum class ShaderStage : uint32_t {
        Vertex,
        Fragment,
        Compute,
    };

    /**
     * @brief Image format enum, mapping to VkFormat.
     *
     * Upper layers use this instead of VkFormat directly.
     * Formats are added as needed; this is not an exhaustive list.
     */
    enum class Format : uint32_t {
        Undefined,

        // 8-bit per channel
        R8Unorm,
        R8G8Unorm,
        R8G8B8A8Unorm,
        R8G8B8A8Srgb,
        B8G8R8A8Unorm,
        B8G8R8A8Srgb,

        // 16-bit per channel (HDR)
        R16Sfloat,
        R16G16Sfloat,
        R16G16B16A16Sfloat,

        // 32-bit per channel
        R32Sfloat,
        R32G32Sfloat,
        R32G32B32A32Sfloat,

        // Packed
        A2B10G10R10UnormPack32,

        // Depth / stencil
        D32Sfloat,
        D24UnormS8Uint,
    };

    /**
     * @brief GPU memory allocation strategy.
     *
     * Maps to VMA memory usage flags.
     */
    enum class MemoryUsage : uint32_t {
        /** @brief Device-local, not CPU-accessible. Best for GPU-only resources. */
        GpuOnly,

        /** @brief CPU-writable, visible to GPU. For staging and dynamic uniforms. */
        CpuToGpu,

        /** @brief GPU-writable, CPU-readable. For readback (e.g. screenshots). */
        GpuToCpu,
    };

    // ---- Format conversion utilities ----

    /**
     * @brief Converts a Format enum value to the corresponding VkFormat.
     *
     * Used by ResourceManager for resource creation and by the Render Graph
     * for barrier construction. Defined alongside the Format enum to keep
     * the mapping in a single location.
     */
    inline VkFormat to_vk_format(const Format format) {
        switch (format) {
            case Format::Undefined: return VK_FORMAT_UNDEFINED;
            case Format::R8Unorm: return VK_FORMAT_R8_UNORM;
            case Format::R8G8Unorm: return VK_FORMAT_R8G8_UNORM;
            case Format::R8G8B8A8Unorm: return VK_FORMAT_R8G8B8A8_UNORM;
            case Format::R8G8B8A8Srgb: return VK_FORMAT_R8G8B8A8_SRGB;
            case Format::B8G8R8A8Unorm: return VK_FORMAT_B8G8R8A8_UNORM;
            case Format::B8G8R8A8Srgb: return VK_FORMAT_B8G8R8A8_SRGB;
            case Format::R16Sfloat: return VK_FORMAT_R16_SFLOAT;
            case Format::R16G16Sfloat: return VK_FORMAT_R16G16_SFLOAT;
            case Format::R16G16B16A16Sfloat: return VK_FORMAT_R16G16B16A16_SFLOAT;
            case Format::R32Sfloat: return VK_FORMAT_R32_SFLOAT;
            case Format::R32G32Sfloat: return VK_FORMAT_R32G32_SFLOAT;
            case Format::R32G32B32A32Sfloat: return VK_FORMAT_R32G32B32A32_SFLOAT;
            case Format::A2B10G10R10UnormPack32: return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
            case Format::D32Sfloat: return VK_FORMAT_D32_SFLOAT;
            case Format::D24UnormS8Uint: return VK_FORMAT_D24_UNORM_S8_UINT;
        }
        return VK_FORMAT_UNDEFINED;
    }

    /**
     * @brief Derives the VkImageAspectFlags from a Format enum value.
     *
     * Depth/stencil formats yield VK_IMAGE_ASPECT_DEPTH_BIT;
     * all other formats yield VK_IMAGE_ASPECT_COLOR_BIT.
     */
    inline VkImageAspectFlags aspect_from_format(const Format format) {
        switch (format) {
            case Format::D32Sfloat:
            case Format::D24UnormS8Uint:
                return VK_IMAGE_ASPECT_DEPTH_BIT;
            default:
                return VK_IMAGE_ASPECT_COLOR_BIT;
        }
    }

} // namespace himalaya::rhi
