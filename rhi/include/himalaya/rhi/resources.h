#pragma once

/**
 * @file resources.h
 * @brief GPU resource management: buffers, images, and resource pool.
 */

#include <himalaya/rhi/types.h>

#include <cstdint>
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

        /** @brief Number of samples per pixel (1 = no MSAA). */
        uint32_t sample_count;

        /** @brief Pixel format. */
        Format format;

        /** @brief Intended image usage (sampled, attachment, transfer, etc.). */
        ImageUsage usage;
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
         * @param desc Buffer creation parameters.
         * @return Handle to the created buffer.
         */
        [[nodiscard]] BufferHandle create_buffer(const BufferDesc &desc);

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
         * @param desc Image creation parameters.
         * @return Handle to the created image.
         */
        [[nodiscard]] ImageHandle create_image(const ImageDesc &desc);

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

        // ---- Upload ----

        /**
         * @brief Uploads CPU data to a GPU-only buffer via a staging buffer.
         *
         * Creates a temporary staging buffer, copies data into it, submits a
         * transfer command, waits for completion, and destroys the staging buffer.
         * Suitable for one-time uploads (e.g. vertex/index data at load time).
         *
         * @param handle  Destination buffer (must have TransferDst usage).
         * @param data    Source data pointer.
         * @param size    Number of bytes to upload.
         * @param offset  Byte offset into the destination buffer.
         */
        void upload_buffer(BufferHandle handle, const void *data, uint64_t size, uint64_t offset = 0);

    private:
        /** @brief Vulkan context (device, allocator, queues). */
        Context *context_ = nullptr;

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
    };
} // namespace himalaya::rhi
