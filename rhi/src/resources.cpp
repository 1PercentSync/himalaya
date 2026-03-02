#include <himalaya/rhi/resources.h>
#include <himalaya/rhi/context.h>

#include <cassert>

#include <spdlog/spdlog.h>

namespace himalaya::rhi {
    void ResourceManager::init(Context *context) {
        context_ = context;
        spdlog::info("Resource manager initialized");
    }

    void ResourceManager::destroy() {
        // Destroy all remaining buffers
        uint32_t leaked_buffers = 0;
        for (auto &buf: buffers_) {
            if (buf.buffer != VK_NULL_HANDLE) {
                vmaDestroyBuffer(context_->allocator, buf.buffer, buf.allocation);
                buf.buffer = VK_NULL_HANDLE;
                ++leaked_buffers;
            }
        }
        buffers_.clear();
        free_buffer_slots_.clear();

        // Destroy all remaining images
        uint32_t leaked_images = 0;
        for (auto &img: images_) {
            if (img.image != VK_NULL_HANDLE) {
                if (img.view != VK_NULL_HANDLE) {
                    vkDestroyImageView(context_->device, img.view, nullptr);
                }
                vmaDestroyImage(context_->allocator, img.image, img.allocation);
                img.image = VK_NULL_HANDLE;
                ++leaked_images;
            }
        }
        images_.clear();
        free_image_slots_.clear();

        if (leaked_buffers > 0 || leaked_images > 0) {
            spdlog::warn("Resource manager destroyed with {} leaked buffer(s) and {} leaked image(s)",
                         leaked_buffers,
                         leaked_images);
        }

        context_ = nullptr;
        spdlog::info("Resource manager destroyed");
    }

    // ---- Pool slot allocation ----

    // Reuses a free slot if available, otherwise appends a new one.
    uint32_t ResourceManager::allocate_buffer_slot() {
        if (!free_buffer_slots_.empty()) {
            const uint32_t index = free_buffer_slots_.back();
            free_buffer_slots_.pop_back();
            return index;
        }
        buffers_.emplace_back();
        return static_cast<uint32_t>(buffers_.size() - 1);
    }

    uint32_t ResourceManager::allocate_image_slot() {
        if (!free_image_slots_.empty()) {
            const uint32_t index = free_image_slots_.back();
            free_image_slots_.pop_back();
            return index;
        }
        images_.emplace_back();
        return static_cast<uint32_t>(images_.size() - 1);
    }

    // ---- Enum conversion helpers ----

    // Translates BufferUsage flags to VkBufferUsageFlags.
    static VkBufferUsageFlags to_vk_buffer_usage(const BufferUsage usage) {
        VkBufferUsageFlags flags = 0;
        if (has_flag(usage, BufferUsage::VertexBuffer)) flags |= VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
        if (has_flag(usage, BufferUsage::IndexBuffer)) flags |= VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
        if (has_flag(usage, BufferUsage::UniformBuffer)) flags |= VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
        if (has_flag(usage, BufferUsage::StorageBuffer)) flags |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        if (has_flag(usage, BufferUsage::TransferSrc)) flags |= VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        if (has_flag(usage, BufferUsage::TransferDst)) flags |= VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        return flags;
    }

    // Translates MemoryUsage to VmaAllocationCreateInfo.
    // VMA_MEMORY_USAGE_AUTO lets VMA pick the best heap; the flags refine CPU access.
    static VmaAllocationCreateInfo to_vma_alloc_info(const MemoryUsage memory) {
        VmaAllocationCreateInfo info{};
        info.usage = VMA_MEMORY_USAGE_AUTO;
        switch (memory) {
            case MemoryUsage::GpuOnly:
                info.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
                break;
            case MemoryUsage::CpuToGpu:
                info.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT
                             | VMA_ALLOCATION_CREATE_MAPPED_BIT;
                break;
            case MemoryUsage::GpuToCpu:
                info.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT
                             | VMA_ALLOCATION_CREATE_MAPPED_BIT;
                break;
        }
        return info;
    }

    // Translates ImageUsage flags to VkImageUsageFlags.
    static VkImageUsageFlags to_vk_image_usage(const ImageUsage usage) {
        VkImageUsageFlags flags = 0;
        if (has_flag(usage, ImageUsage::Sampled)) flags |= VK_IMAGE_USAGE_SAMPLED_BIT;
        if (has_flag(usage, ImageUsage::Storage)) flags |= VK_IMAGE_USAGE_STORAGE_BIT;
        if (has_flag(usage, ImageUsage::ColorAttachment)) flags |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        if (has_flag(usage, ImageUsage::DepthAttachment)) flags |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
        if (has_flag(usage, ImageUsage::TransferSrc)) flags |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        if (has_flag(usage, ImageUsage::TransferDst)) flags |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        return flags;
    }

    // Translates Format enum to VkFormat.
    static VkFormat to_vk_format(const Format format) {
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
            case Format::D32Sfloat: return VK_FORMAT_D32_SFLOAT;
            case Format::D24UnormS8Uint: return VK_FORMAT_D24_UNORM_S8_UINT;
        }
        return VK_FORMAT_UNDEFINED;
    }

    // Returns the appropriate aspect mask for an image view based on format.
    static VkImageAspectFlags aspect_from_format(const Format format) {
        switch (format) {
            case Format::D32Sfloat:
            case Format::D24UnormS8Uint:
                return VK_IMAGE_ASPECT_DEPTH_BIT;
            default:
                return VK_IMAGE_ASPECT_COLOR_BIT;
        }
    }

    // ---- Buffer operations ----

    BufferHandle ResourceManager::create_buffer(const BufferDesc &desc) {
        assert(desc.size > 0 && "Buffer size must be greater than zero");

        VkBufferCreateInfo buffer_info{};
        buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        buffer_info.size = desc.size;
        buffer_info.usage = to_vk_buffer_usage(desc.usage);
        buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        const auto alloc_info = to_vma_alloc_info(desc.memory);

        const uint32_t index = allocate_buffer_slot();
        // ReSharper disable once CppUseStructuredBinding
        auto &slot = buffers_[index];

        VK_CHECK(vmaCreateBuffer(context_->allocator,
            &buffer_info,
            &alloc_info,
            &slot.buffer,
            &slot.allocation,
            &slot.allocation_info));
        slot.desc = desc;

        return {index, slot.generation};
    }

    void ResourceManager::destroy_buffer(const BufferHandle handle) {
        assert(handle.valid() && "Invalid buffer handle");
        assert(handle.index < buffers_.size() && "Buffer handle index out of range");

        auto &slot = buffers_[handle.index];
        assert(slot.generation == handle.generation && "Stale buffer handle (use-after-free)");
        assert(slot.buffer != VK_NULL_HANDLE && "Double-free on buffer slot");

        vmaDestroyBuffer(context_->allocator, slot.buffer, slot.allocation);
        slot.buffer = VK_NULL_HANDLE;
        slot.allocation = VK_NULL_HANDLE;
        slot.allocation_info = {};
        ++slot.generation;
        free_buffer_slots_.push_back(handle.index);
    }

    const Buffer &ResourceManager::get_buffer(const BufferHandle handle) const {
        assert(handle.valid() && "Invalid buffer handle");
        assert(handle.index < buffers_.size() && "Buffer handle index out of range");
        assert(buffers_[handle.index].generation == handle.generation
            && "Stale buffer handle (use-after-free)");
        return buffers_[handle.index];
    }

    // ---- Image operations ----

    ImageHandle ResourceManager::create_image(const ImageDesc &desc) {
        assert(desc.width > 0 && desc.height > 0 && "Image dimensions must be greater than zero");

        VkImageCreateInfo image_info{};
        image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        image_info.imageType = VK_IMAGE_TYPE_2D;
        image_info.format = to_vk_format(desc.format);
        image_info.extent = {desc.width, desc.height, desc.depth};
        image_info.mipLevels = desc.mip_levels;
        image_info.arrayLayers = 1;
        image_info.samples = static_cast<VkSampleCountFlagBits>(desc.sample_count);
        image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
        image_info.usage = to_vk_image_usage(desc.usage);
        image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        // Images are always GPU-only in M1
        VmaAllocationCreateInfo alloc_info{};
        alloc_info.usage = VMA_MEMORY_USAGE_AUTO;
        alloc_info.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;

        const uint32_t index = allocate_image_slot();
        // ReSharper disable once CppUseStructuredBinding
        auto &slot = images_[index];

        VK_CHECK(vmaCreateImage(context_->allocator,
            &image_info,
            &alloc_info,
            &slot.image,
            &slot.allocation,
            nullptr));
        slot.desc = desc;

        // Create default image view
        VkImageViewCreateInfo view_info{};
        view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view_info.image = slot.image;
        view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view_info.format = image_info.format;
        view_info.subresourceRange.aspectMask = aspect_from_format(desc.format);
        view_info.subresourceRange.baseMipLevel = 0;
        view_info.subresourceRange.levelCount = desc.mip_levels;
        view_info.subresourceRange.baseArrayLayer = 0;
        view_info.subresourceRange.layerCount = 1;

        VK_CHECK(vkCreateImageView(context_->device, &view_info, nullptr, &slot.view));

        return {index, slot.generation};
    }

    void ResourceManager::destroy_image(const ImageHandle handle) {
        assert(handle.valid() && "Invalid image handle");
        assert(handle.index < images_.size() && "Image handle index out of range");

        auto &slot = images_[handle.index];
        assert(slot.generation == handle.generation && "Stale image handle (use-after-free)");
        assert(slot.image != VK_NULL_HANDLE && "Double-free on image slot");

        if (slot.view != VK_NULL_HANDLE) {
            vkDestroyImageView(context_->device, slot.view, nullptr);
            slot.view = VK_NULL_HANDLE;
        }
        vmaDestroyImage(context_->allocator, slot.image, slot.allocation);
        slot.image = VK_NULL_HANDLE;
        slot.allocation = VK_NULL_HANDLE;
        ++slot.generation;
        free_image_slots_.push_back(handle.index);
    }

    const Image &ResourceManager::get_image(const ImageHandle handle) const {
        assert(handle.valid() && "Invalid image handle");
        assert(handle.index < images_.size() && "Image handle index out of range");
        assert(images_[handle.index].generation == handle.generation
            && "Stale image handle (use-after-free)");
        return images_[handle.index];
    }

    // ---- Upload ----

    void ResourceManager::upload_buffer(const BufferHandle handle,
                                        const void *data,
                                        const uint64_t size,
                                        const uint64_t offset) const {
        assert(data && "Upload source data must not be null");
        assert(size > 0 && "Upload size must be greater than zero");

        const auto &dst = get_buffer(handle);
        assert(has_flag(dst.desc.usage, BufferUsage::TransferDst)
            && "Destination buffer must have TransferDst usage");
        assert(offset + size <= dst.desc.size && "Upload exceeds buffer bounds");

        // 1. Create a temporary CPU-visible staging buffer
        VkBufferCreateInfo staging_buffer_info{};
        staging_buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        staging_buffer_info.size = size;
        staging_buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        staging_buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        VmaAllocationCreateInfo staging_alloc_info{};
        staging_alloc_info.usage = VMA_MEMORY_USAGE_AUTO;
        staging_alloc_info.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT
                                   | VMA_ALLOCATION_CREATE_MAPPED_BIT;

        VkBuffer staging_buffer = VK_NULL_HANDLE;
        VmaAllocation staging_allocation = VK_NULL_HANDLE;
        VmaAllocationInfo staging_info{};

        VK_CHECK(vmaCreateBuffer(context_->allocator,
            &staging_buffer_info,
            &staging_alloc_info,
            &staging_buffer,
            &staging_allocation,
            &staging_info));

        // 2. Copy data into the mapped staging buffer
        std::memcpy(staging_info.pMappedData, data, size);

        // 3. Allocate and begin a one-shot command buffer
        VkCommandBufferAllocateInfo cmd_alloc_info{};
        cmd_alloc_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cmd_alloc_info.commandPool = context_->current_frame().command_pool;
        cmd_alloc_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cmd_alloc_info.commandBufferCount = 1;

        VkCommandBuffer cmd = VK_NULL_HANDLE;
        VK_CHECK(vkAllocateCommandBuffers(context_->device, &cmd_alloc_info, &cmd));

        VkCommandBufferBeginInfo begin_info{};
        begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        VK_CHECK(vkBeginCommandBuffer(cmd, &begin_info));

        // 4. Record buffer copy command
        VkBufferCopy copy_region{};
        copy_region.srcOffset = 0;
        copy_region.dstOffset = offset;
        copy_region.size = size;
        vkCmdCopyBuffer(cmd, staging_buffer, dst.buffer, 1, &copy_region);

        VK_CHECK(vkEndCommandBuffer(cmd));

        // 5. Submit and wait for completion
        VkSubmitInfo submit_info{};
        submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit_info.commandBufferCount = 1;
        submit_info.pCommandBuffers = &cmd;

        VK_CHECK(vkQueueSubmit(context_->graphics_queue, 1, &submit_info, VK_NULL_HANDLE));
        VK_CHECK(vkQueueWaitIdle(context_->graphics_queue));

        // 6. Cleanup: free command buffer and destroy staging buffer
        vkFreeCommandBuffers(context_->device,
                             context_->current_frame().command_pool, 1, &cmd);
        vmaDestroyBuffer(context_->allocator, staging_buffer, staging_allocation);
    }
} // namespace himalaya::rhi
