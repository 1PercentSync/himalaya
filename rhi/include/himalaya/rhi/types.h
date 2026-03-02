#pragma once

/**
 * @file types.h
 * @brief RHI type definitions: resource handles, shader stages, and format enums.
 */

#include <cstdint>

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
     * @brief Pipeline resource handle.
     * @see ImageHandle for generation-based semantics.
     */
    struct PipelineHandle {
        uint32_t index = UINT32_MAX;
        uint32_t generation = 0;

        [[nodiscard]] bool valid() const { return index != UINT32_MAX; }
        bool operator==(const PipelineHandle &) const = default;
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

} // namespace himalaya::rhi
