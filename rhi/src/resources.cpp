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
                         leaked_buffers, leaked_images);
        }

        context_ = nullptr;
        spdlog::info("Resource manager destroyed");
    }

    BufferHandle ResourceManager::create_buffer([[maybe_unused]] const BufferDesc &desc) {
        // Stub — implemented in "Buffer 创建接口" task
        assert(false && "create_buffer not yet implemented");
        return {};
    }

    void ResourceManager::destroy_buffer([[maybe_unused]] BufferHandle handle) {
        // Stub — implemented in "Buffer 创建接口" task
        assert(false && "destroy_buffer not yet implemented");
    }

    const Buffer &ResourceManager::get_buffer(BufferHandle handle) const {
        // Stub — implemented in "资源池实现" task
        assert(handle.valid() && handle.index < buffers_.size());
        return buffers_[handle.index];
    }

    ImageHandle ResourceManager::create_image([[maybe_unused]] const ImageDesc &desc) {
        // Stub — implemented in "Image 创建接口" task
        assert(false && "create_image not yet implemented");
        return {};
    }

    void ResourceManager::destroy_image([[maybe_unused]] ImageHandle handle) {
        // Stub — implemented in "Image 创建接口" task
        assert(false && "destroy_image not yet implemented");
    }

    const Image &ResourceManager::get_image(ImageHandle handle) const {
        // Stub — implemented in "资源池实现" task
        assert(handle.valid() && handle.index < images_.size());
        return images_[handle.index];
    }

    void ResourceManager::upload_buffer([[maybe_unused]] BufferHandle handle,
                                        [[maybe_unused]] const void *data,
                                        [[maybe_unused]] uint64_t size,
                                        [[maybe_unused]] uint64_t offset) {
        // Stub — implemented in "Staging buffer 上传流程" task
        assert(false && "upload_buffer not yet implemented");
    }
} // namespace himalaya::rhi
