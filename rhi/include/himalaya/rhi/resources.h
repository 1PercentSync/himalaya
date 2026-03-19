#pragma once

/**
 * @file resources.h
 * @brief GPU resource management: buffers, images, and resource pool.
 */

#include <himalaya/rhi/types.h>

#include <span>
#include <vector>

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>

namespace himalaya::rhi {
    class Context;

    // ---- Usage Flags ----

    /** @brief Buffer usage flags, mapping to VkBufferUsageFlags. */
    enum class BufferUsage : uint32_t {
        VertexBuffer = 1 << 0,
        IndexBuffer = 1 << 1,
        UniformBuffer = 1 << 2,
        StorageBuffer = 1 << 3,
        TransferSrc = 1 << 4,
        TransferDst = 1 << 5,
    };

    /** @brief Bitwise OR for BufferUsage flags. */
    inline BufferUsage operator|(BufferUsage a, BufferUsage b) {
        return static_cast<BufferUsage>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
    }

    /** @brief Compound bitwise OR for BufferUsage flags. */
    inline BufferUsage &operator|=(BufferUsage &a, BufferUsage b) {
        return a = a | b;
    }

    /** @brief Bitwise AND for BufferUsage flags. */
    inline BufferUsage operator&(BufferUsage a, BufferUsage b) {
        return static_cast<BufferUsage>(static_cast<uint32_t>(a) & static_cast<uint32_t>(b));
    }

    /** @brief Tests whether a specific BufferUsage flag bit is set. */
    inline bool has_flag(BufferUsage flags, BufferUsage bit) {
        return (static_cast<uint32_t>(flags) & static_cast<uint32_t>(bit)) != 0;
    }

    /** @brief Image usage flags, mapping to VkImageUsageFlags. */
    enum class ImageUsage : uint32_t {
        Sampled = 1 << 0,
        Storage = 1 << 1,
        ColorAttachment = 1 << 2,
        DepthAttachment = 1 << 3,
        TransferSrc = 1 << 4,
        TransferDst = 1 << 5,
    };

    /** @brief Bitwise OR for ImageUsage flags. */
    inline ImageUsage operator|(ImageUsage a, ImageUsage b) {
        return static_cast<ImageUsage>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
    }

    /** @brief Compound bitwise OR for ImageUsage flags. */
    inline ImageUsage &operator|=(ImageUsage &a, ImageUsage b) {
        return a = a | b;
    }

    /** @brief Bitwise AND for ImageUsage flags. */
    inline ImageUsage operator&(ImageUsage a, ImageUsage b) {
        return static_cast<ImageUsage>(static_cast<uint32_t>(a) & static_cast<uint32_t>(b));
    }

    /** @brief Tests whether a specific ImageUsage flag bit is set. */
    inline bool has_flag(ImageUsage flags, ImageUsage bit) {
        return (static_cast<uint32_t>(flags) & static_cast<uint32_t>(bit)) != 0;
    }

    // ---- Sampler Enums ----

    /** @brief Texture filtering mode. */
    enum class Filter : uint32_t {
        Nearest,
        Linear,
    };

    /** @brief Mipmap interpolation mode. */
    enum class SamplerMipMode : uint32_t {
        Nearest,
        Linear,
    };

    /** @brief Texture address wrapping mode. */
    enum class SamplerWrapMode : uint32_t {
        Repeat,
        ClampToEdge,
        MirroredRepeat,
    };

    // ---- Resource Descriptions ----

    /**
     * @brief Description for creating a GPU buffer.
     *
     * Upper layers fill this struct and pass it to ResourceManager::create_buffer().
     * The memory strategy is selected via the memory field.
     */
    struct BufferDesc {
        /** @brief Buffer size in bytes. */
        uint64_t size;

        /** @brief Intended buffer usage (vertex, index, uniform, etc.). */
        BufferUsage usage;

        /** @brief Memory allocation strategy (GPU-only, CPU-visible, etc.). */
        MemoryUsage memory;
    };

    /**
     * @brief Description for creating a GPU image.
     *
     * Upper layers fill this struct and pass it to ResourceManager::create_image().
     */
    struct ImageDesc {
        /** @brief Image width in pixels. */
        uint32_t width;

        /** @brief Image height in pixels. */
        uint32_t height;

        /** @brief Image depth (1 for 2D images). */
        uint32_t depth;

        /** @brief Number of mip levels. */
        uint32_t mip_levels;

        /** @brief Number of array layers (6 for cubemaps, 1 for regular 2D). */
        uint32_t array_layers;

        /** @brief Number of samples per pixel (1 = no MSAA). */
        uint32_t sample_count;

        /** @brief Pixel format. */
        Format format;

        /** @brief Intended image usage (sampled, attachment, transfer, etc.). */
        ImageUsage usage;
    };

    /**
     * @brief Description for creating a GPU sampler.
     *
     * Upper layers fill this struct and pass it to ResourceManager::create_sampler().
     */
    struct SamplerDesc {
        /** @brief Magnification filter. */
        Filter mag_filter;

        /** @brief Minification filter. */
        Filter min_filter;

        /** @brief Mipmap interpolation mode. */
        SamplerMipMode mip_mode;

        /** @brief Horizontal wrap mode. */
        SamplerWrapMode wrap_u;

        /** @brief Vertical wrap mode. */
        SamplerWrapMode wrap_v;

        /** @brief Max anisotropy level (0 = disabled). */
        float max_anisotropy;

        /** @brief Maximum mip level accessible (0 = base level only, VK_LOD_CLAMP_NONE = no clamping). */
        float max_lod;

        /** @brief Enable depth comparison for shadow sampling. */
        bool compare_enable;

        /** @brief Comparison operation when compare_enable is true. */
        CompareOp compare_op;
    };

    // ---- Internal Resource Storage ----

    /**
     * @brief Internal buffer slot: Vulkan handle + VMA allocation + metadata.
     *
     * Stored in the resource pool. The generation counter enables
     * use-after-free detection on BufferHandle lookups.
     */
    struct Buffer {
        /** @brief Vulkan buffer handle (VK_NULL_HANDLE if slot is free). */
        VkBuffer buffer = VK_NULL_HANDLE;

        /** @brief VMA allocation backing the buffer memory. */
        VmaAllocation allocation = VK_NULL_HANDLE;

        /** @brief VMA allocation info (contains mapped pointer for CPU-visible buffers). */
        VmaAllocationInfo allocation_info{};

        /** @brief Creation parameters (size, usage, memory strategy). */
        BufferDesc desc{};

        /** @brief Generation counter for use-after-free detection. */
        uint32_t generation = 0;
    };

    /**
     * @brief Internal image slot: Vulkan handle + view + VMA allocation + metadata.
     *
     * Stored in the resource pool. The generation counter enables
     * use-after-free detection on ImageHandle lookups.
     */
    struct Image {
        /** @brief Vulkan image handle (VK_NULL_HANDLE if slot is free). */
        VkImage image = VK_NULL_HANDLE;

        /** @brief Default image view (full mip chain, all layers). */
        VkImageView view = VK_NULL_HANDLE;

        /** @brief VMA allocation backing the image memory. */
        VmaAllocation allocation = VK_NULL_HANDLE;

        /** @brief Creation parameters (dimensions, format, usage). */
        ImageDesc desc{};

        /** @brief Generation counter for use-after-free detection. */
        uint32_t generation = 0;
    };

    /**
     * @brief Internal sampler slot: Vulkan sampler handle + metadata.
     *
     * Stored in the resource pool. The generation counter enables
     * use-after-free detection on SamplerHandle lookups.
     */
    struct Sampler {
        /** @brief Vulkan sampler handle (VK_NULL_HANDLE if slot is free). */
        VkSampler sampler = VK_NULL_HANDLE;

        /** @brief Creation parameters (filter, wrap, anisotropy). */
        SamplerDesc desc{};

        /** @brief Generation counter for use-after-free detection. */
        uint32_t generation = 0;
    };

    // ---- Resource Manager ----

    /**
     * @brief Manages GPU buffer and image resources with generation-based handles.
     *
     * Resources are allocated from slot pools. Each slot tracks a generation counter
     * that increments on destruction, invalidating all existing handles to that slot.
     * This provides use-after-free detection at the cost of a single uint32_t comparison.
     *
     * Lifetime is managed explicitly via init() and destroy().
     * destroy() releases all remaining resources; individual resources can be
     * released earlier via destroy_buffer() / destroy_image().
     */
    class ResourceManager {
    public:
        /**
         * @brief Initializes the resource manager.
         * @param context Vulkan context providing device and allocator.
         */
        void init(Context *context);

        /**
         * @brief Destroys all remaining resources and resets internal state.
         *
         * Must be called before the Vulkan device is destroyed.
         */
        void destroy();

        // ---- Buffer operations ----

        /**
         * @brief Creates a GPU buffer.
         * @param desc       Buffer creation parameters.
         * @param debug_name Human-readable name for validation/RenderDoc (must not be null).
         * @return Handle to the created buffer.
         */
        [[nodiscard]] BufferHandle create_buffer(const BufferDesc &desc, const char *debug_name);

        /**
         * @brief Destroys a buffer and frees its GPU memory.
         * @param handle Buffer to destroy. The handle becomes invalid after this call.
         */
        void destroy_buffer(BufferHandle handle);

        /**
         * @brief Returns the internal buffer data for a valid handle.
         * @param handle Buffer handle (must be valid and current generation).
         * @return Reference to the internal Buffer slot.
         */
        [[nodiscard]] const Buffer &get_buffer(BufferHandle handle) const;

        // ---- Image operations ----

        /**
         * @brief Creates a GPU image with a default image view.
         * @param desc       Image creation parameters.
         * @param debug_name Human-readable name for validation/RenderDoc (must not be null).
         * @return Handle to the created image.
         */
        [[nodiscard]] ImageHandle create_image(const ImageDesc &desc, const char *debug_name);

        /**
         * @brief Destroys an image, its view, and frees its GPU memory.
         * @param handle Image to destroy. The handle becomes invalid after this call.
         */
        void destroy_image(ImageHandle handle);

        /**
         * @brief Returns the internal image data for a valid handle.
         * @param handle Image handle (must be valid and current generation).
         * @return Reference to the internal Image slot.
         */
        [[nodiscard]] const Image &get_image(ImageHandle handle) const;

        /**
         * @brief Registers an externally created image (e.g. swapchain image).
         *
         * Allocates a slot to record the VkImage/VkImageView/desc without owning
         * the VMA allocation. The caller is responsible for the image's lifetime.
         *
         * @param image Vulkan image handle.
         * @param view  Vulkan image view.
         * @param desc  Image description (format, dimensions, usage).
         * @return Handle to reference this image through the resource system.
         */
        [[nodiscard]] ImageHandle register_external_image(VkImage image, VkImageView view, const ImageDesc &desc);

        /**
         * @brief Unregisters an external image previously registered.
         *
         * Releases the slot and increments generation. Does NOT destroy the
         * VkImage or VkImageView (they are externally owned).
         *
         * @param handle Handle returned by register_external_image().
         */
        void unregister_external_image(ImageHandle handle);

        // ---- Sampler operations ----

        /**
         * @brief Creates a GPU sampler.
         * @param desc       Sampler creation parameters.
         * @param debug_name Human-readable name for validation/RenderDoc (must not be null).
         * @return Handle to the created sampler.
         */
        [[nodiscard]] SamplerHandle create_sampler(const SamplerDesc &desc, const char *debug_name);

        /**
         * @brief Destroys a sampler.
         * @param handle Sampler to destroy. The handle becomes invalid after this call.
         */
        void destroy_sampler(SamplerHandle handle);

        /**
         * @brief Returns the internal sampler data for a valid handle.
         * @param handle Sampler handle (must be valid and current generation).
         * @return Reference to the internal Sampler slot.
         */
        [[nodiscard]] const Sampler &get_sampler(SamplerHandle handle) const;

        /** @brief Returns the device-supported maximum sampler anisotropy. */
        [[nodiscard]] float max_sampler_anisotropy() const;

        // ---- Upload ----

        /**
         * @brief Records a staging-buffer copy into the active immediate command scope.
         *
         * Must be called within a Context::begin_immediate() / end_immediate() scope.
         * Creates a staging buffer, copies data into it, and records a vkCmdCopyBuffer.
         * The staging buffer is registered with Context for deferred cleanup.
         *
         * @param handle  Destination buffer (must have TransferDst usage).
         * @param data    Source data pointer.
         * @param size    Number of bytes to upload.
         * @param offset  Byte offset into the destination buffer.
         */
        void upload_buffer(BufferHandle handle, const void *data, uint64_t size, uint64_t offset = 0) const;

        /**
         * @brief Records staging-buffer-to-image copy into the active immediate scope.
         *
         * Must be called within a Context::begin_immediate() / end_immediate() scope.
         * Transitions the image from UNDEFINED to TRANSFER_DST, copies mip level 0,
         * then transitions to SHADER_READ_ONLY. The staging buffer is registered
         * with Context for deferred cleanup.
         *
         * @param handle    Destination image (must have TransferDst usage).
         * @param data      Source pixel data pointer.
         * @param size      Number of bytes to upload.
         * @param dst_stage Pipeline stage that will first consume the image
         *                  (e.g. FRAGMENT_SHADER_BIT for textures, COMPUTE_SHADER_BIT for IBL).
         */
        void upload_image(ImageHandle handle,
                          const void *data,
                          uint64_t size,
                          VkPipelineStageFlags2 dst_stage) const;

        /** @brief Describes one mip level region for upload_image_all_levels(). */
        struct MipUploadRegion {
            uint64_t buffer_offset; ///< Byte offset of this level's data within the source buffer.
            uint32_t width;         ///< Pixel width of this mip level.
            uint32_t height;        ///< Pixel height of this mip level.
        };

        /**
         * @brief Uploads a pre-built mip chain to an image (2D or cubemap).
         *
         * Must be called within a Context::begin_immediate() / end_immediate() scope.
         * Creates a single staging buffer, records one VkBufferImageCopy2 per mip level,
         * then transitions the image to SHADER_READ_ONLY. For cubemaps (array_layers=6),
         * each level's data must contain all 6 faces contiguous — matching KTX2 layout.
         *
         * @param handle      Destination image (must have TransferDst usage).
         * @param data        Source data containing all mip levels.
         * @param total_size  Total byte size of data.
         * @param mip_regions Per-level descriptors; mip_regions[0] = base level.
         * @param dst_stage   Pipeline stage that will first consume the image.
         */
        void upload_image_all_levels(ImageHandle handle,
                                     const void *data,
                                     uint64_t total_size,
                                     std::span<const MipUploadRegion> mip_regions,
                                     VkPipelineStageFlags2 dst_stage) const;

        /**
         * @brief Records mip generation commands into the active immediate scope.
         *
         * Must be called within a Context::begin_immediate() / end_immediate() scope.
         * Expects mip level 0 in SHADER_READ_ONLY layout (after upload_image).
         * Uses vkCmdBlitImage with linear filtering for each level.
         * Image must have TransferSrc and TransferDst usage flags and more than 1 mip level.
         *
         * @param handle Image with populated mip level 0.
         */
        void generate_mips(ImageHandle handle) const;

    private:
        /** @brief Vulkan context (device, allocator, queues). */
        Context *context_ = nullptr;

        /** @brief vkSetDebugUtilsObjectNameEXT function pointer (null if extension unavailable). */
        PFN_vkSetDebugUtilsObjectNameEXT pfn_set_debug_name_ = nullptr;

        /**
         * @brief Assigns a debug name to a Vulkan object via VK_EXT_debug_utils.
         *
         * No-op if the extension function pointer was not loaded (release builds
         * where VK_EXT_debug_utils is not enabled).
         *
         * @param type   Vulkan object type enum.
         * @param handle Raw Vulkan handle cast to uint64_t.
         * @param name   Null-terminated name string.
         */
        void set_debug_name(VkObjectType type, uint64_t handle, const char *name) const;

        // ---- Pool slot allocation ----

        /**
         * @brief Allocates a buffer slot, reusing a free slot if available.
         * @return Index of the allocated slot in buffers_.
         */
        uint32_t allocate_buffer_slot();

        /**
         * @brief Allocates an image slot, reusing a free slot if available.
         * @return Index of the allocated slot in images_.
         */
        uint32_t allocate_image_slot();

        /**
         * @brief Allocates a sampler slot, reusing a free slot if available.
         * @return Index of the allocated slot in samplers_.
         */
        uint32_t allocate_sampler_slot();

        // ---- Buffer pool ----

        /** @brief All buffer slots (active and free). */
        std::vector<Buffer> buffers_;

        /** @brief Indices of free buffer slots available for reuse. */
        std::vector<uint32_t> free_buffer_slots_;

        // ---- Image pool ----

        /** @brief All image slots (active and free). */
        std::vector<Image> images_;

        /** @brief Indices of free image slots available for reuse. */
        std::vector<uint32_t> free_image_slots_;

        // ---- Sampler pool ----

        /** @brief All sampler slots (active and free). */
        std::vector<Sampler> samplers_;

        /** @brief Indices of free sampler slots available for reuse. */
        std::vector<uint32_t> free_sampler_slots_;
    };
} // namespace himalaya::rhi
