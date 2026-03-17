#include <himalaya/rhi/resources.h>
#include <himalaya/rhi/context.h>

#include <cassert>
#include <string>

#include <spdlog/spdlog.h>

namespace himalaya::rhi {
    void ResourceManager::init(Context *context) {
        context_ = context;
        pfn_set_debug_name_ = reinterpret_cast<PFN_vkSetDebugUtilsObjectNameEXT>(
            vkGetInstanceProcAddr(context_->instance, "vkSetDebugUtilsObjectNameEXT"));
        spdlog::info("Resource manager initialized");
    }

    void ResourceManager::set_debug_name(const VkObjectType type, const uint64_t handle, const char *name) const {
        if (!pfn_set_debug_name_) return;
        VkDebugUtilsObjectNameInfoEXT info{};
        info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT;
        info.objectType = type;
        info.objectHandle = handle;
        info.pObjectName = name;
        pfn_set_debug_name_(context_->device, &info);
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

        // Destroy all remaining images (skip externally owned images)
        uint32_t leaked_images = 0;
        for (auto &img: images_) {
            if (img.image != VK_NULL_HANDLE) {
                if (img.allocation != VK_NULL_HANDLE) {
                    // Owned image: destroy view, image, and VMA allocation
                    if (img.view != VK_NULL_HANDLE) {
                        vkDestroyImageView(context_->device, img.view, nullptr);
                    }
                    vmaDestroyImage(context_->allocator, img.image, img.allocation);
                } else {
                    // External image: only release the slot, do not destroy Vulkan objects
                    spdlog::warn("External image slot still registered at destroy time "
                        "(forgot to call unregister_external_image?)");
                }
                img.image = VK_NULL_HANDLE;
                ++leaked_images;
            }
        }
        images_.clear();
        free_image_slots_.clear();

        // Destroy all remaining samplers
        uint32_t leaked_samplers = 0;
        for (auto &smp: samplers_) {
            if (smp.sampler != VK_NULL_HANDLE) {
                vkDestroySampler(context_->device, smp.sampler, nullptr);
                smp.sampler = VK_NULL_HANDLE;
                ++leaked_samplers;
            }
        }
        samplers_.clear();
        free_sampler_slots_.clear();

        if (leaked_buffers > 0 || leaked_images > 0 || leaked_samplers > 0) {
            spdlog::warn(
                "Resource manager destroyed with {} leaked buffer(s), {} leaked image(s), {} leaked sampler(s)",
                leaked_buffers,
                leaked_images,
                leaked_samplers);
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

    uint32_t ResourceManager::allocate_sampler_slot() {
        if (!free_sampler_slots_.empty()) {
            const uint32_t index = free_sampler_slots_.back();
            free_sampler_slots_.pop_back();
            return index;
        }
        samplers_.emplace_back();
        return static_cast<uint32_t>(samplers_.size() - 1);
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
                // No flags: VMA auto-selects pooled or dedicated allocation
                // based on VkMemoryDedicatedRequirements from the driver.
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

    // to_vk_format() and aspect_from_format() are defined as public inline
    // functions in rhi/types.h (included transitively via rhi/resources.h).

    // Translates Filter to VkFilter.
    static VkFilter to_vk_filter(const Filter filter) {
        switch (filter) {
            case Filter::Nearest: return VK_FILTER_NEAREST;
            case Filter::Linear: return VK_FILTER_LINEAR;
        }
        return VK_FILTER_LINEAR;
    }

    // Translates SamplerMipMode to VkSamplerMipmapMode.
    static VkSamplerMipmapMode to_vk_mip_mode(const SamplerMipMode mode) {
        switch (mode) {
            case SamplerMipMode::Nearest: return VK_SAMPLER_MIPMAP_MODE_NEAREST;
            case SamplerMipMode::Linear: return VK_SAMPLER_MIPMAP_MODE_LINEAR;
        }
        return VK_SAMPLER_MIPMAP_MODE_LINEAR;
    }

    // Translates SamplerWrapMode to VkSamplerAddressMode.
    static VkSamplerAddressMode to_vk_wrap_mode(const SamplerWrapMode mode) {
        switch (mode) {
            case SamplerWrapMode::Repeat: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
            case SamplerWrapMode::ClampToEdge: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            case SamplerWrapMode::MirroredRepeat: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
        }
        return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    }

    // ---- Buffer operations ----

    BufferHandle ResourceManager::create_buffer(const BufferDesc &desc, const char *debug_name) {
        assert(desc.size > 0 && "Buffer size must be greater than zero");
        assert(debug_name && "debug_name must not be null");

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

        set_debug_name(VK_OBJECT_TYPE_BUFFER, reinterpret_cast<uint64_t>(slot.buffer), debug_name);

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

    ImageHandle ResourceManager::create_image(const ImageDesc &desc, const char *debug_name) {
        assert(desc.width > 0 && desc.height > 0 && "Image dimensions must be greater than zero");
        assert(desc.depth > 0 && "Image depth must be greater than zero");
        assert(desc.depth == 1 &&
            "3D images (depth > 1) not yet supported — remove this assert when adding VK_IMAGE_TYPE_3D");
        assert(desc.mip_levels > 0 && "Image mip_levels must be greater than zero");
        assert(desc.array_layers > 0 && "Image array_layers must be greater than zero");
        assert(desc.sample_count > 0 && "Image sample_count must be greater than zero");
        assert(debug_name && "debug_name must not be null");

        VkImageCreateInfo image_info{};
        image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        image_info.imageType = VK_IMAGE_TYPE_2D;
        image_info.format = to_vk_format(desc.format);
        image_info.extent = {desc.width, desc.height, desc.depth};
        image_info.mipLevels = desc.mip_levels;
        image_info.arrayLayers = desc.array_layers;
        image_info.samples = static_cast<VkSampleCountFlagBits>(desc.sample_count);
        image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
        image_info.usage = to_vk_image_usage(desc.usage);
        image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        const bool is_cubemap = desc.array_layers == 6;
        if (is_cubemap) {
            image_info.flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
        }

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

        // Create default image view (cube view for cubemaps, 2D otherwise)
        VkImageViewCreateInfo view_info{};
        view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view_info.image = slot.image;
        view_info.viewType = is_cubemap ? VK_IMAGE_VIEW_TYPE_CUBE : VK_IMAGE_VIEW_TYPE_2D;
        view_info.format = image_info.format;
        view_info.subresourceRange.aspectMask = aspect_from_format(desc.format);
        view_info.subresourceRange.baseMipLevel = 0;
        view_info.subresourceRange.levelCount = desc.mip_levels;
        view_info.subresourceRange.baseArrayLayer = 0;
        view_info.subresourceRange.layerCount = desc.array_layers;

        VK_CHECK(vkCreateImageView(context_->device, &view_info, nullptr, &slot.view));

        set_debug_name(VK_OBJECT_TYPE_IMAGE, reinterpret_cast<uint64_t>(slot.image), debug_name);
        const std::string view_name = std::string(debug_name) + " [View]";
        set_debug_name(VK_OBJECT_TYPE_IMAGE_VIEW, reinterpret_cast<uint64_t>(slot.view), view_name.c_str());

        return {index, slot.generation};
    }

    void ResourceManager::destroy_image(const ImageHandle handle) {
        assert(handle.valid() && "Invalid image handle");
        assert(handle.index < images_.size() && "Image handle index out of range");

        auto &slot = images_[handle.index];
        assert(slot.generation == handle.generation && "Stale image handle (use-after-free)");
        assert(slot.image != VK_NULL_HANDLE && "Double-free on image slot");
        assert(slot.allocation != VK_NULL_HANDLE
            && "Cannot destroy_image on an external image (use unregister_external_image instead)");

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

    // ReSharper disable CppParameterMayBeConst
    ImageHandle ResourceManager::register_external_image(VkImage image, VkImageView view, const ImageDesc &desc) {
        // ReSharper restore CppParameterMayBeConst
        const uint32_t index = allocate_image_slot();
        // ReSharper disable once CppUseStructuredBinding
        auto &slot = images_[index];
        slot.image = image;
        slot.view = view;
        slot.allocation = VK_NULL_HANDLE; // Not owned by VMA
        slot.desc = desc;
        return {index, slot.generation};
    }

    void ResourceManager::unregister_external_image(const ImageHandle handle) {
        assert(handle.valid() && "Invalid image handle");
        assert(handle.index < images_.size() && "Image handle index out of range");

        auto &slot = images_[handle.index];
        assert(slot.generation == handle.generation && "Stale image handle (use-after-free)");
        assert(slot.image != VK_NULL_HANDLE && "Double-free on image slot");

        // Do NOT destroy VkImage/VkImageView — externally owned
        slot.image = VK_NULL_HANDLE;
        slot.view = VK_NULL_HANDLE;
        slot.allocation = VK_NULL_HANDLE;
        ++slot.generation;
        free_image_slots_.push_back(handle.index);
    }

    // ---- Sampler operations ----

    SamplerHandle ResourceManager::create_sampler(const SamplerDesc &desc, const char *debug_name) {
        assert(debug_name && "debug_name must not be null");

        VkSamplerCreateInfo sampler_info{};
        sampler_info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        sampler_info.magFilter = to_vk_filter(desc.mag_filter);
        sampler_info.minFilter = to_vk_filter(desc.min_filter);
        sampler_info.mipmapMode = to_vk_mip_mode(desc.mip_mode);
        sampler_info.addressModeU = to_vk_wrap_mode(desc.wrap_u);
        sampler_info.addressModeV = to_vk_wrap_mode(desc.wrap_v);
        sampler_info.addressModeW = to_vk_wrap_mode(desc.wrap_u);
        sampler_info.maxLod = desc.max_lod;

        if (desc.max_anisotropy > 0.0f) {
            sampler_info.anisotropyEnable = VK_TRUE;
            sampler_info.maxAnisotropy = desc.max_anisotropy;
        }

        const uint32_t index = allocate_sampler_slot();
        // ReSharper disable once CppUseStructuredBinding
        auto &slot = samplers_[index];

        VK_CHECK(vkCreateSampler(context_->device, &sampler_info, nullptr, &slot.sampler));
        slot.desc = desc;

        set_debug_name(VK_OBJECT_TYPE_SAMPLER, reinterpret_cast<uint64_t>(slot.sampler), debug_name);

        return {index, slot.generation};
    }

    float ResourceManager::max_sampler_anisotropy() const {
        return context_->max_sampler_anisotropy;
    }

    void ResourceManager::destroy_sampler(const SamplerHandle handle) {
        assert(handle.valid() && "Invalid sampler handle");
        assert(handle.index < samplers_.size() && "Sampler handle index out of range");

        auto &slot = samplers_[handle.index];
        assert(slot.generation == handle.generation && "Stale sampler handle (use-after-free)");
        assert(slot.sampler != VK_NULL_HANDLE && "Double-free on sampler slot");

        vkDestroySampler(context_->device, slot.sampler, nullptr);
        slot.sampler = VK_NULL_HANDLE;
        ++slot.generation;
        free_sampler_slots_.push_back(handle.index);
    }

    const Sampler &ResourceManager::get_sampler(const SamplerHandle handle) const {
        assert(handle.valid() && "Invalid sampler handle");
        assert(handle.index < samplers_.size() && "Sampler handle index out of range");
        assert(samplers_[handle.index].generation == handle.generation && "Stale sampler handle (use-after-free)");
        return samplers_[handle.index];
    }

    // ---- Upload ----

    void ResourceManager::upload_buffer(const BufferHandle handle,
                                        const void *data,
                                        const uint64_t size,
                                        const uint64_t offset) const {
        assert(context_->is_immediate_active()
            && "upload_buffer must be called within begin_immediate/end_immediate scope");
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

        // 3. Record copy command into the active immediate command buffer
        VkBufferCopy copy_region{};
        copy_region.srcOffset = 0;
        copy_region.dstOffset = offset;
        copy_region.size = size;
        vkCmdCopyBuffer(context_->immediate_command_buffer,
                        staging_buffer,
                        dst.buffer,
                        1,
                        &copy_region);

        // 4. Register staging buffer for deferred cleanup at end_immediate()
        context_->push_staging_buffer(staging_buffer, staging_allocation);
    }

    void ResourceManager::upload_image(const ImageHandle handle,
                                       const void *data,
                                       const uint64_t size,
                                       const VkPipelineStageFlags2 dst_stage) const {
        assert(context_->is_immediate_active()
            && "upload_image must be called within begin_immediate/end_immediate scope");
        assert(data && "Upload source data must not be null");
        assert(size > 0 && "Upload size must be greater than zero");

        const auto &dst = get_image(handle);
        assert(has_flag(dst.desc.usage, ImageUsage::TransferDst)
            && "Destination image must have TransferDst usage");

        // 1. Create staging buffer
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

        std::memcpy(staging_info.pMappedData, data, size);

        // 2. Record commands into the active immediate command buffer
        // ReSharper disable once CppLocalVariableMayBeConst
        VkCommandBuffer cmd = context_->immediate_command_buffer;
        const VkImageAspectFlags aspect = aspect_from_format(dst.desc.format);

        // Transition mip 0: UNDEFINED -> TRANSFER_DST_OPTIMAL
        VkImageMemoryBarrier2 to_transfer{};
        to_transfer.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        to_transfer.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
        to_transfer.srcAccessMask = VK_ACCESS_2_NONE;
        to_transfer.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        to_transfer.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        to_transfer.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        to_transfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        to_transfer.image = dst.image;
        to_transfer.subresourceRange = {aspect, 0, 1, 0, 1};

        VkDependencyInfo dep{};
        dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dep.imageMemoryBarrierCount = 1;
        dep.pImageMemoryBarriers = &to_transfer;
        vkCmdPipelineBarrier2(cmd, &dep);

        // Copy staging buffer to image mip 0
        VkBufferImageCopy region{};
        region.bufferOffset = 0;
        region.bufferRowLength = 0;
        region.bufferImageHeight = 0;
        region.imageSubresource = {aspect, 0, 0, 1};
        region.imageOffset = {0, 0, 0};
        region.imageExtent = {dst.desc.width, dst.desc.height, 1};
        vkCmdCopyBufferToImage(cmd, staging_buffer, dst.image,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

        // Transition mip 0: TRANSFER_DST_OPTIMAL -> SHADER_READ_ONLY_OPTIMAL
        VkImageMemoryBarrier2 to_read{};
        to_read.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        to_read.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        to_read.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        to_read.dstStageMask = dst_stage;
        to_read.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        to_read.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        to_read.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        to_read.image = dst.image;
        to_read.subresourceRange = {aspect, 0, 1, 0, 1};

        dep.pImageMemoryBarriers = &to_read;
        vkCmdPipelineBarrier2(cmd, &dep);

        // 3. Register staging buffer for deferred cleanup at end_immediate()
        context_->push_staging_buffer(staging_buffer, staging_allocation);
    }

    void ResourceManager::upload_image_all_levels(const ImageHandle handle,
                                                  const void *data,
                                                  const uint64_t total_size,
                                                  const std::span<const MipUploadRegion> mip_regions,
                                                  const VkPipelineStageFlags2 dst_stage) const {
        assert(context_->is_immediate_active()
            && "upload_image_all_levels must be called within begin_immediate/end_immediate scope");
        assert(data && "Upload source data must not be null");
        assert(total_size > 0 && "Upload size must be greater than zero");
        assert(!mip_regions.empty() && "Must provide at least one mip region");

        const auto &dst = get_image(handle);
        assert(has_flag(dst.desc.usage, ImageUsage::TransferDst)
            && "Destination image must have TransferDst usage");
        assert(mip_regions.size() == dst.desc.mip_levels
            && "mip_regions count must match image mip_levels");

        // 1. Create staging buffer
        VkBufferCreateInfo staging_buffer_info{};
        staging_buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        staging_buffer_info.size = total_size;
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

        std::memcpy(staging_info.pMappedData, data, total_size);

        // 2. Record commands
        // ReSharper disable once CppLocalVariableMayBeConst
        VkCommandBuffer cmd = context_->immediate_command_buffer;
        const VkImageAspectFlags aspect = aspect_from_format(dst.desc.format);
        const uint32_t layer_count = dst.desc.array_layers;

        // Transition all subresources: UNDEFINED -> TRANSFER_DST_OPTIMAL
        VkImageMemoryBarrier2 to_transfer{};
        to_transfer.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        to_transfer.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
        to_transfer.srcAccessMask = VK_ACCESS_2_NONE;
        to_transfer.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        to_transfer.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        to_transfer.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        to_transfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        to_transfer.image = dst.image;
        to_transfer.subresourceRange = {
            aspect,
            0,
            static_cast<uint32_t>(mip_regions.size()),
            0,
            layer_count
        };

        VkDependencyInfo dep{};
        dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dep.imageMemoryBarrierCount = 1;
        dep.pImageMemoryBarriers = &to_transfer;
        vkCmdPipelineBarrier2(cmd, &dep);

        // Build copy regions — one per mip level, each covering all array layers
        std::vector<VkBufferImageCopy2> regions(mip_regions.size());
        for (size_t i = 0; i < mip_regions.size(); ++i) {
            auto &r = regions[i];
            r.sType = VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2;
            r.pNext = nullptr;
            r.bufferOffset = mip_regions[i].buffer_offset;
            r.bufferRowLength = 0;
            r.bufferImageHeight = 0;
            r.imageSubresource = {aspect, static_cast<uint32_t>(i), 0, layer_count};
            r.imageOffset = {0, 0, 0};
            r.imageExtent = {mip_regions[i].width, mip_regions[i].height, 1};
        }

        VkCopyBufferToImageInfo2 copy_info{};
        copy_info.sType = VK_STRUCTURE_TYPE_COPY_BUFFER_TO_IMAGE_INFO_2;
        copy_info.srcBuffer = staging_buffer;
        copy_info.dstImage = dst.image;
        copy_info.dstImageLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        copy_info.regionCount = static_cast<uint32_t>(regions.size());
        copy_info.pRegions = regions.data();
        vkCmdCopyBufferToImage2(cmd, &copy_info);

        // Transition all subresources: TRANSFER_DST_OPTIMAL -> SHADER_READ_ONLY_OPTIMAL
        VkImageMemoryBarrier2 to_read{};
        to_read.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        to_read.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        to_read.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        to_read.dstStageMask = dst_stage;
        to_read.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        to_read.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        to_read.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        to_read.image = dst.image;
        to_read.subresourceRange = {
            aspect,
            0,
            static_cast<uint32_t>(mip_regions.size()),
            0,
            layer_count
        };

        dep.pImageMemoryBarriers = &to_read;
        vkCmdPipelineBarrier2(cmd, &dep);

        // 3. Register staging buffer for deferred cleanup
        context_->push_staging_buffer(staging_buffer, staging_allocation);
    }

    void ResourceManager::generate_mips(const ImageHandle handle) const {
        assert(context_->is_immediate_active()
            && "generate_mips must be called within begin_immediate/end_immediate scope");

        const auto &img = get_image(handle);
        assert(has_flag(img.desc.usage, ImageUsage::TransferSrc)
            && "Image must have TransferSrc usage for mip generation");
        assert(has_flag(img.desc.usage, ImageUsage::TransferDst)
            && "Image must have TransferDst usage for mip generation");
        assert(img.desc.mip_levels > 1 && "Image must have more than 1 mip level");

        const VkImageAspectFlags aspect = aspect_from_format(img.desc.format);

        // Record into the active immediate command buffer
        // ReSharper disable once CppLocalVariableMayBeConst
        VkCommandBuffer cmd = context_->immediate_command_buffer;

        const uint32_t layers = img.desc.array_layers;

        // Transition mip 0: SHADER_READ_ONLY -> TRANSFER_SRC (blit source)
        VkImageMemoryBarrier2 barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        barrier.srcStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        barrier.srcAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        barrier.dstStageMask = VK_PIPELINE_STAGE_2_BLIT_BIT;
        barrier.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barrier.image = img.image;
        barrier.subresourceRange = {aspect, 0, 1, 0, layers};

        VkDependencyInfo dep{};
        dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dep.imageMemoryBarrierCount = 1;
        dep.pImageMemoryBarriers = &barrier;
        vkCmdPipelineBarrier2(cmd, &dep);

        auto mip_width = static_cast<int32_t>(img.desc.width);
        auto mip_height = static_cast<int32_t>(img.desc.height);

        for (uint32_t i = 1; i < img.desc.mip_levels; ++i) {
            const int32_t next_width = std::max(mip_width / 2, 1);
            const int32_t next_height = std::max(mip_height / 2, 1);

            // Transition mip i: UNDEFINED -> TRANSFER_DST (blit destination)
            barrier.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
            barrier.srcAccessMask = VK_ACCESS_2_NONE;
            barrier.dstStageMask = VK_PIPELINE_STAGE_2_BLIT_BIT;
            barrier.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            barrier.subresourceRange = {aspect, i, 1, 0, layers};
            vkCmdPipelineBarrier2(cmd, &dep);

            // Blit from mip i-1 to mip i
            VkImageBlit2 blit{};
            blit.sType = VK_STRUCTURE_TYPE_IMAGE_BLIT_2;
            blit.srcSubresource = {aspect, i - 1, 0, layers};
            blit.srcOffsets[0] = {0, 0, 0};
            blit.srcOffsets[1] = {mip_width, mip_height, 1};
            blit.dstSubresource = {aspect, i, 0, layers};
            blit.dstOffsets[0] = {0, 0, 0};
            blit.dstOffsets[1] = {next_width, next_height, 1};

            VkBlitImageInfo2 blit_info{};
            blit_info.sType = VK_STRUCTURE_TYPE_BLIT_IMAGE_INFO_2;
            blit_info.srcImage = img.image;
            blit_info.srcImageLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            blit_info.dstImage = img.image;
            blit_info.dstImageLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            blit_info.regionCount = 1;
            blit_info.pRegions = &blit;
            blit_info.filter = VK_FILTER_LINEAR;
            vkCmdBlitImage2(cmd, &blit_info);

            // Transition mip i: TRANSFER_DST -> TRANSFER_SRC (source for next level)
            barrier.srcStageMask = VK_PIPELINE_STAGE_2_BLIT_BIT;
            barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            barrier.dstStageMask = VK_PIPELINE_STAGE_2_BLIT_BIT;
            barrier.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
            barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            barrier.subresourceRange = {aspect, i, 1, 0, layers};
            vkCmdPipelineBarrier2(cmd, &dep);

            mip_width = next_width;
            mip_height = next_height;
        }

        // Transition all mip levels: TRANSFER_SRC -> SHADER_READ_ONLY
        barrier.srcStageMask = VK_PIPELINE_STAGE_2_BLIT_BIT;
        barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
        barrier.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        barrier.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.subresourceRange = {aspect, 0, img.desc.mip_levels, 0, layers};
        vkCmdPipelineBarrier2(cmd, &dep);
    }
} // namespace himalaya::rhi
