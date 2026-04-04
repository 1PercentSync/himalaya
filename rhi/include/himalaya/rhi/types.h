#pragma once

/**
 * @file types.h
 * @brief RHI type definitions: resource handles, shader stages, and format enums.
 */

#include <cstdint>
#include <utility>

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
        RayGen,
        ClosestHit,
        AnyHit,
        Miss,
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

        // 16-bit per channel
        R16G16Unorm,

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
        B10G11R11UfloatPack32,

        // Block-compressed (4x4 texel blocks, 16 bytes per block)
        Bc5UnormBlock,
        Bc6hUfloatBlock,
        Bc7UnormBlock,
        Bc7SrgbBlock,

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

    /**
     * @brief Depth/stencil comparison operation.
     *
     * Used by comparison samplers (e.g. shadow map sampling) and depth test
     * configuration. Wraps VkCompareOp without exposing Vulkan types.
     */
    enum class CompareOp : uint32_t {
        Never,
        Less,
        Equal,
        LessOrEqual,
        Greater,
        GreaterOrEqual,
    };

    // ---- Alignment utilities ----

    /**
     * @brief Rounds up a value to the next multiple of a power-of-two alignment.
     * @param value     Value to align.
     * @param alignment Must be a power of two.
     * @return Smallest multiple of alignment that is >= value.
     */
    template<typename T>
    constexpr T align_up(T value, T alignment) {
        return (value + alignment - 1) & ~(alignment - 1);
    }

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
            case Format::R16G16Unorm: return VK_FORMAT_R16G16_UNORM;
            case Format::R16Sfloat: return VK_FORMAT_R16_SFLOAT;
            case Format::R16G16Sfloat: return VK_FORMAT_R16G16_SFLOAT;
            case Format::R16G16B16A16Sfloat: return VK_FORMAT_R16G16B16A16_SFLOAT;
            case Format::R32Sfloat: return VK_FORMAT_R32_SFLOAT;
            case Format::R32G32Sfloat: return VK_FORMAT_R32G32_SFLOAT;
            case Format::R32G32B32A32Sfloat: return VK_FORMAT_R32G32B32A32_SFLOAT;
            case Format::A2B10G10R10UnormPack32: return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
            case Format::B10G11R11UfloatPack32: return VK_FORMAT_B10G11R11_UFLOAT_PACK32;
            case Format::Bc5UnormBlock: return VK_FORMAT_BC5_UNORM_BLOCK;
            case Format::Bc6hUfloatBlock: return VK_FORMAT_BC6H_UFLOAT_BLOCK;
            case Format::Bc7UnormBlock: return VK_FORMAT_BC7_UNORM_BLOCK;
            case Format::Bc7SrgbBlock: return VK_FORMAT_BC7_SRGB_BLOCK;
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

    /**
     * @brief Converts a VkFormat value back to the corresponding Format enum.
     *
     * Returns Format::Undefined for unrecognized VkFormat values.
     * Used by KTX2 reader to map the file's vkFormat field to Format.
     */
    inline Format from_vk_format(const VkFormat vk_format) {
        switch (vk_format) {
            case VK_FORMAT_R8_UNORM: return Format::R8Unorm;
            case VK_FORMAT_R8G8_UNORM: return Format::R8G8Unorm;
            case VK_FORMAT_R8G8B8A8_UNORM: return Format::R8G8B8A8Unorm;
            case VK_FORMAT_R8G8B8A8_SRGB: return Format::R8G8B8A8Srgb;
            case VK_FORMAT_B8G8R8A8_UNORM: return Format::B8G8R8A8Unorm;
            case VK_FORMAT_B8G8R8A8_SRGB: return Format::B8G8R8A8Srgb;
            case VK_FORMAT_R16G16_UNORM: return Format::R16G16Unorm;
            case VK_FORMAT_R16_SFLOAT: return Format::R16Sfloat;
            case VK_FORMAT_R16G16_SFLOAT: return Format::R16G16Sfloat;
            case VK_FORMAT_R16G16B16A16_SFLOAT: return Format::R16G16B16A16Sfloat;
            case VK_FORMAT_R32_SFLOAT: return Format::R32Sfloat;
            case VK_FORMAT_R32G32_SFLOAT: return Format::R32G32Sfloat;
            case VK_FORMAT_R32G32B32A32_SFLOAT: return Format::R32G32B32A32Sfloat;
            case VK_FORMAT_A2B10G10R10_UNORM_PACK32: return Format::A2B10G10R10UnormPack32;
            case VK_FORMAT_B10G11R11_UFLOAT_PACK32: return Format::B10G11R11UfloatPack32;
            case VK_FORMAT_BC5_UNORM_BLOCK: return Format::Bc5UnormBlock;
            case VK_FORMAT_BC6H_UFLOAT_BLOCK: return Format::Bc6hUfloatBlock;
            case VK_FORMAT_BC7_UNORM_BLOCK: return Format::Bc7UnormBlock;
            case VK_FORMAT_BC7_SRGB_BLOCK: return Format::Bc7SrgbBlock;
            case VK_FORMAT_D32_SFLOAT: return Format::D32Sfloat;
            case VK_FORMAT_D24_UNORM_S8_UINT: return Format::D24UnormS8Uint;
            default: return Format::Undefined;
        }
    }

    // ---- CompareOp conversion ----

    /**
     * @brief Converts a CompareOp enum value to the corresponding VkCompareOp.
     *
     * Used by sampler creation for comparison samplers (e.g. shadow mapping).
     */
    inline VkCompareOp to_vk_compare_op(const CompareOp op) {
        switch (op) {
            case CompareOp::Never: return VK_COMPARE_OP_NEVER;
            case CompareOp::Less: return VK_COMPARE_OP_LESS;
            case CompareOp::Equal: return VK_COMPARE_OP_EQUAL;
            case CompareOp::LessOrEqual: return VK_COMPARE_OP_LESS_OR_EQUAL;
            case CompareOp::Greater: return VK_COMPARE_OP_GREATER;
            case CompareOp::GreaterOrEqual: return VK_COMPARE_OP_GREATER_OR_EQUAL;
        }
        return VK_COMPARE_OP_NEVER;
    }

    // ---- Format property utilities ----

    /**
     * @brief Returns the byte size of one texel block for the given format.
     *
     * For block-compressed formats (BC5/BC6H/BC7), a block is 4x4 texels = 16 bytes.
     * For uncompressed formats, a "block" is a single texel.
     */
    inline uint32_t format_bytes_per_block(const Format format) {
        switch (format) {
            case Format::R8Unorm: return 1;
            case Format::R8G8Unorm: return 2;
            case Format::R8G8B8A8Unorm:
            case Format::R8G8B8A8Srgb:
            case Format::B8G8R8A8Unorm:
            case Format::B8G8R8A8Srgb: return 4;
            case Format::R16G16Unorm: return 4;
            case Format::R16Sfloat: return 2;
            case Format::R16G16Sfloat: return 4;
            case Format::R16G16B16A16Sfloat: return 8;
            case Format::R32Sfloat: return 4;
            case Format::R32G32Sfloat: return 8;
            case Format::R32G32B32A32Sfloat: return 16;
            case Format::A2B10G10R10UnormPack32: return 4;
            case Format::B10G11R11UfloatPack32: return 4;
            case Format::Bc5UnormBlock:
            case Format::Bc6hUfloatBlock:
            case Format::Bc7UnormBlock:
            case Format::Bc7SrgbBlock: return 16;
            case Format::D32Sfloat: return 4;
            case Format::D24UnormS8Uint: return 4;
            default: return 0;
        }
    }

    /**
     * @brief Returns the texel block extent {width, height} for the given format.
     *
     * Block-compressed formats cover 4x4 texels per block.
     * Uncompressed formats have a 1x1 block extent.
     */
    inline std::pair<uint32_t, uint32_t> format_block_extent(const Format format) {
        switch (format) {
            case Format::Bc5UnormBlock:
            case Format::Bc6hUfloatBlock:
            case Format::Bc7UnormBlock:
            case Format::Bc7SrgbBlock: return {4, 4};
            default: return {1, 1};
        }
    }

    /** @brief Returns true if the format is block-compressed (BC). */
    inline bool format_is_block_compressed(const Format format) {
        switch (format) {
            case Format::Bc5UnormBlock:
            case Format::Bc6hUfloatBlock:
            case Format::Bc7UnormBlock:
            case Format::Bc7SrgbBlock: return true;
            default: return false;
        }
    }
} // namespace himalaya::rhi
