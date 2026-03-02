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

    // ---- Buffer operations ----

    BufferHandle ResourceManager::create_buffer([[maybe_unused]] const BufferDesc &desc) {
        // Stub — VMA buffer creation implemented in "Buffer 创建接口" task
        assert(false && "create_buffer not yet implemented");
        return {};
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

    ImageHandle ResourceManager::create_image([[maybe_unused]] const ImageDesc &desc) {
        // Stub — VMA image creation implemented in "Image 创建接口" task
        assert(false && "create_image not yet implemented");
        return {};
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

    void ResourceManager::upload_buffer([[maybe_unused]] BufferHandle handle,
                                        [[maybe_unused]] const void *data,
                                        [[maybe_unused]] uint64_t size,
                                        [[maybe_unused]] uint64_t offset) {
        // Stub — implemented in "Staging buffer 上传流程" task
        assert(false && "upload_buffer not yet implemented");
    }
} // namespace himalaya::rhi
