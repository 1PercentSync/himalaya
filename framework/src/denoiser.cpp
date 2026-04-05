/**
 * @file denoiser.cpp
 * @brief OIDN-based asynchronous denoiser implementation.
 */

#include <himalaya/framework/denoiser.h>
#include <himalaya/rhi/context.h>
#include <himalaya/rhi/resources.h>

#include <OpenImageDenoise/oidn.hpp>
#include <spdlog/spdlog.h>

namespace himalaya::framework {
    constexpr size_t kBeautyBytesPerPixel = 16; // RGBA32F
    constexpr size_t kAuxBytesPerPixel = 8; // RGBA16F

    /** @brief PIMPL holding all OIDN objects, hidden from the header. */
    struct Denoiser::OidnImpl {
        oidn::DeviceRef device;
        oidn::FilterRef filter;
        oidn::BufferRef beauty_buf;
        oidn::BufferRef albedo_buf;
        oidn::BufferRef normal_buf;
        oidn::BufferRef output_buf;
    };

    // Destructor must be defined in the .cpp where OidnImpl is complete
    // so unique_ptr's deleter can see the full type.
    Denoiser::Denoiser() = default;

    Denoiser::~Denoiser() = default;

    void Denoiser::init(const rhi::Context &ctx, rhi::ResourceManager &rm, const uint32_t width,
                        const uint32_t height) {
        device_ = ctx.device;
        allocator_ = ctx.allocator;
        rm_ = &rm;
        width_ = width;
        height_ = height;

        // ---- OIDN device (GPU preferred, CPU fallback) ----
        // OIDN picks the best available device automatically (GPU preferred).
        // On multi-GPU systems it may select a different GPU than Vulkan renders on.
        // This is acceptable: data goes through CPU staging either way, and offloading
        // denoise to a separate GPU avoids contention with the PT workload.
        // OIDN 2.x does not support selecting a specific GPU by index.
        oidn_ = std::make_unique<OidnImpl>();
        oidn_->device = oidn::newDevice();
        oidn_->device.commit();

        if (const auto device_type = oidn_->device.get<oidn::DeviceType>("type");
            device_type == oidn::DeviceType::CPU) {
            spdlog::warn("OIDN: using CPU device (~25x slower than GPU)");
        } else {
            auto type_name = "GPU";
            if (device_type == oidn::DeviceType::CUDA) {
                type_name = "CUDA GPU";
            } else if (device_type == oidn::DeviceType::HIP) {
                type_name = "HIP GPU";
            } else if (device_type == oidn::DeviceType::SYCL) {
                type_name = "SYCL GPU";
            }
            spdlog::info("OIDN: using {} device", type_name);
        }

        // ---- OIDN filter ("RT" with albedo + normal auxiliary) ----
        oidn_->filter = oidn_->device.newFilter("RT");
        oidn_->filter.set("hdr", true);
        oidn_->filter.set("cleanAux", true);
        oidn_->filter.set("quality", oidn::Quality::High);

        // ---- OIDN buffers (device-managed, host-accessible) ----
        const size_t beauty_size = static_cast<size_t>(width) * height * kBeautyBytesPerPixel; // RGBA32F
        const size_t aux_size = static_cast<size_t>(width) * height * kAuxBytesPerPixel; // RGBA16F

        oidn_->beauty_buf = oidn_->device.newBuffer(beauty_size);
        oidn_->albedo_buf = oidn_->device.newBuffer(aux_size);
        oidn_->normal_buf = oidn_->device.newBuffer(aux_size);
        oidn_->output_buf = oidn_->device.newBuffer(beauty_size);

        oidn_->filter.setImage("color", oidn_->beauty_buf, oidn::Format::Float3,
                               width, height, 0, kBeautyBytesPerPixel, 0);
        oidn_->filter.setImage("albedo", oidn_->albedo_buf, oidn::Format::Half3,
                               width, height, 0, kAuxBytesPerPixel, 0);
        oidn_->filter.setImage("normal", oidn_->normal_buf, oidn::Format::Half3,
                               width, height, 0, kAuxBytesPerPixel, 0);
        oidn_->filter.setImage("output", oidn_->output_buf, oidn::Format::Float3,
                               width, height, 0, kBeautyBytesPerPixel, 0);
        oidn_->filter.commit();

        // ---- Vulkan staging buffers ----
        create_staging_buffers(rm, width, height);

        // ---- Timeline semaphore ----
        VkSemaphoreTypeCreateInfo type_info{};
        type_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
        type_info.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
        type_info.initialValue = 0;

        VkSemaphoreCreateInfo sem_info{};
        sem_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        sem_info.pNext = &type_info;

        VK_CHECK(vkCreateSemaphore(device_, &sem_info, nullptr, &timeline_semaphore_));
        semaphore_value_ = 0;
    }

    void Denoiser::request_denoise(const uint32_t accumulation_generation) {
        trigger_generation_ = accumulation_generation;
        ++semaphore_value_;
        state_.store(DenoiseState::ReadbackPending, std::memory_order_release);
        spdlog::info("OIDN: denoise requested (gen={})", accumulation_generation);
    }

    Denoiser::SemaphoreSignal Denoiser::launch_processing() {
        state_.store(DenoiseState::Processing, std::memory_order_release);

        // Capture by value what the thread needs; pointers to persistent resources are safe.
        // ReSharper disable once CppLocalVariableMayBeConst
        VkDevice vk_device = device_;
        // ReSharper disable once CppLocalVariableMayBeConst
        VkSemaphore semaphore = timeline_semaphore_;
        const uint64_t wait_value = semaphore_value_;
        const uint32_t w = width_;
        const uint32_t h = height_;

        auto *impl = oidn_.get();

        // Mapped pointers for Vulkan staging buffers (persistently mapped by VMA).
        // SAFETY: these raw pointers are valid for the lifetime of their VmaAllocation.
        // All paths that destroy staging buffers (on_resize, destroy) call join_and_idle()
        // first, ensuring the thread has finished before any pointer becomes dangling.
        const auto &rb_beauty_data = rm_->get_buffer(readback_beauty_);
        const auto &rb_albedo_data = rm_->get_buffer(readback_albedo_);
        const auto &rb_normal_data = rm_->get_buffer(readback_normal_);
        const auto &upload_data = rm_->get_buffer(upload_);

        const void *readback_beauty_ptr = rb_beauty_data.allocation_info.pMappedData;
        const void *readback_albedo_ptr = rb_albedo_data.allocation_info.pMappedData;
        const void *readback_normal_ptr = rb_normal_data.allocation_info.pMappedData;
        void *upload_ptr = upload_data.allocation_info.pMappedData;

        // VMA allocator + allocations for cache coherency operations.
        // vmaInvalidateAllocation / vmaFlushAllocation are pure CPU cache ops,
        // safe to call from any thread without external synchronization.
        // ReSharper disable once CppLocalVariableMayBeConst
        VmaAllocator vma = allocator_;
        VmaAllocation rb_beauty_alloc = rb_beauty_data.allocation;
        VmaAllocation rb_albedo_alloc = rb_albedo_data.allocation;
        VmaAllocation rb_normal_alloc = rb_normal_data.allocation;
        VmaAllocation upload_alloc = upload_data.allocation;

        std::atomic<DenoiseState> *state_ptr = &state_;

        thread_ = std::jthread([=](const std::stop_token &stop) {
            // Wait for GPU readback copy to complete.
            // Use a bounded timeout loop so jthread stop requests (from
            // join_and_idle / destroy) can break out even if the driver
            // never signals (e.g. device lost without returning an error).
            VkSemaphoreWaitInfo wait_info{};
            wait_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
            wait_info.semaphoreCount = 1;
            wait_info.pSemaphores = &semaphore;
            wait_info.pValues = &wait_value;

            for (;;) {
                constexpr uint64_t kWaitTimeoutNs = 100'000'000;
                const VkResult result = vkWaitSemaphores(vk_device, &wait_info, kWaitTimeoutNs);
                if (result == VK_SUCCESS) {
                    break;
                }
                if (result == VK_TIMEOUT) {
                    if (stop.stop_requested()) {
                        spdlog::warn("OIDN: denoise thread cancelled during semaphore wait");
                        state_ptr->store(DenoiseState::Idle, std::memory_order_release);
                        return;
                    }
                    continue; // retry
                }
                // Any other error (e.g. VK_ERROR_DEVICE_LOST)
                spdlog::error("OIDN: vkWaitSemaphores failed ({})", static_cast<int>(result));
                state_ptr->store(DenoiseState::Idle, std::memory_order_release);
                return;
            }

            // Invalidate readback buffers for CPU cache coherency.
            // GpuToCpu memory may be HOST_CACHED but not HOST_COHERENT
            // (e.g. AMD discrete GPUs). Without invalidation, CPU may read
            // stale cache lines after GPU writes. No-op on coherent memory.
            vmaInvalidateAllocation(vma, rb_beauty_alloc, 0, VK_WHOLE_SIZE);
            vmaInvalidateAllocation(vma, rb_albedo_alloc, 0, VK_WHOLE_SIZE);
            vmaInvalidateAllocation(vma, rb_normal_alloc, 0, VK_WHOLE_SIZE);

            const size_t beauty_size = static_cast<size_t>(w) * h * kBeautyBytesPerPixel;
            const size_t aux_size = static_cast<size_t>(w) * h * kAuxBytesPerPixel;

            // Copy Vulkan staging → OIDN buffers
            impl->beauty_buf.write(0, beauty_size, readback_beauty_ptr);
            impl->albedo_buf.write(0, aux_size, readback_albedo_ptr);
            impl->normal_buf.write(0, aux_size, readback_normal_ptr);

            // Execute OIDN filter
            impl->filter.execute();
            impl->device.sync();

            // Check for errors
            const char *error_msg = nullptr;
            if (const auto error = impl->device.getError(error_msg); error != oidn::Error::None) {
                spdlog::error("OIDN filter failed: {} ({})",
                              error_msg ? error_msg : "unknown",
                              static_cast<int>(error));
                state_ptr->store(DenoiseState::Idle, std::memory_order_release);
                return;
            }

            // Copy OIDN output → Vulkan upload staging
            impl->output_buf.read(0, beauty_size, upload_ptr);

            // Flush upload buffer for CPU cache coherency.
            // CpuToGpu memory may not be HOST_COHERENT on all platforms.
            // Flush ensures CPU writes are visible to the GPU before the
            // next frame's upload pass reads the buffer. No-op on coherent memory.
            vmaFlushAllocation(vma, upload_alloc, 0, VK_WHOLE_SIZE);

            spdlog::info("OIDN: filter complete, upload pending");
            state_ptr->store(DenoiseState::UploadPending, std::memory_order_release);
        });

        return {timeline_semaphore_, semaphore_value_};
    }

    bool Denoiser::poll_upload_ready(const uint32_t current_generation) {
        if (state_.load(std::memory_order_acquire) != DenoiseState::UploadPending) {
            return false;
        }
        if (current_generation != trigger_generation_) {
            // Accumulation was reset — discard stale result
            spdlog::info("OIDN: discarding stale result (gen {} vs current {})",
                         trigger_generation_, current_generation);
            state_.store(DenoiseState::Idle, std::memory_order_release);
            return false;
        }
        return true;
    }

    void Denoiser::complete_upload() {
        spdlog::info("OIDN: upload complete, displaying denoised result");
        state_.store(DenoiseState::Idle, std::memory_order_release);
    }

    DenoiseState Denoiser::state() const {
        return state_.load(std::memory_order_acquire);
    }

    void Denoiser::on_resize(rhi::ResourceManager &rm, const uint32_t width, const uint32_t height) {
        join_and_idle();

        width_ = width;
        height_ = height;

        // Rebuild Vulkan staging buffers for new resolution
        destroy_staging_buffers(rm);
        create_staging_buffers(rm, width, height);

        // Rebuild OIDN buffers and reconfigure filter for new resolution.
        // Assignment to BufferRef releases the old buffer via reference counting.
        const size_t beauty_size = static_cast<size_t>(width) * height * kBeautyBytesPerPixel;
        const size_t aux_size = static_cast<size_t>(width) * height * kAuxBytesPerPixel;

        oidn_->beauty_buf = oidn_->device.newBuffer(beauty_size);
        oidn_->albedo_buf = oidn_->device.newBuffer(aux_size);
        oidn_->normal_buf = oidn_->device.newBuffer(aux_size);
        oidn_->output_buf = oidn_->device.newBuffer(beauty_size);

        oidn_->filter.setImage("color", oidn_->beauty_buf, oidn::Format::Float3,
                               width, height, 0, kBeautyBytesPerPixel, 0);
        oidn_->filter.setImage("albedo", oidn_->albedo_buf, oidn::Format::Half3,
                               width, height, 0, kAuxBytesPerPixel, 0);
        oidn_->filter.setImage("normal", oidn_->normal_buf, oidn::Format::Half3,
                               width, height, 0, kAuxBytesPerPixel, 0);
        oidn_->filter.setImage("output", oidn_->output_buf, oidn::Format::Float3,
                               width, height, 0, kBeautyBytesPerPixel, 0);
        oidn_->filter.commit();
    }

    void Denoiser::abort() {
        join_and_idle();
    }

    void Denoiser::destroy() {
        join_and_idle();

        // OIDN resources — unique_ptr handles deletion
        oidn_.reset();

        // Vulkan staging buffers
        if (rm_) {
            destroy_staging_buffers(*rm_);
        }

        // Timeline semaphore
        if (timeline_semaphore_ != VK_NULL_HANDLE) {
            vkDestroySemaphore(device_, timeline_semaphore_, nullptr);
            timeline_semaphore_ = VK_NULL_HANDLE;
        }

        device_ = VK_NULL_HANDLE;
        rm_ = nullptr;
    }

    rhi::BufferHandle Denoiser::readback_beauty_buffer() const { return readback_beauty_; }
    rhi::BufferHandle Denoiser::readback_albedo_buffer() const { return readback_albedo_; }
    rhi::BufferHandle Denoiser::readback_normal_buffer() const { return readback_normal_; }
    rhi::BufferHandle Denoiser::upload_buffer() const { return upload_; }
    uint32_t Denoiser::width() const { return width_; }
    uint32_t Denoiser::height() const { return height_; }

    // INVARIANT: must be called before destroying staging buffers or OIDN resources.
    // The background thread captures raw mapped pointers and OIDN object pointers
    // that become dangling after destruction. All callers (on_resize, destroy, abort)
    // call this first.
    void Denoiser::join_and_idle() {
        thread_ = {};
        state_.store(DenoiseState::Idle, std::memory_order_release);
    }

    void Denoiser::create_staging_buffers(rhi::ResourceManager &rm,
                                          const uint32_t w, const uint32_t h) {
        const uint64_t beauty_size = static_cast<uint64_t>(w) * h * kBeautyBytesPerPixel;
        const uint64_t aux_size = static_cast<uint64_t>(w) * h * kAuxBytesPerPixel;

        // Readback: GPU → CPU (GpuToCpu for host-readable after vkCmdCopyImageToBuffer)
        readback_beauty_ = rm.create_buffer(
            {beauty_size, rhi::BufferUsage::TransferDst, rhi::MemoryUsage::GpuToCpu},
            "OIDN Readback Beauty");
        readback_albedo_ = rm.create_buffer(
            {aux_size, rhi::BufferUsage::TransferDst, rhi::MemoryUsage::GpuToCpu},
            "OIDN Readback Albedo");
        readback_normal_ = rm.create_buffer(
            {aux_size, rhi::BufferUsage::TransferDst, rhi::MemoryUsage::GpuToCpu},
            "OIDN Readback Normal");

        // Upload: CPU → GPU (CpuToGpu for host-writable before vkCmdCopyBufferToImage)
        upload_ = rm.create_buffer(
            {beauty_size, rhi::BufferUsage::TransferSrc, rhi::MemoryUsage::CpuToGpu},
            "OIDN Upload Denoised");
    }

    void Denoiser::destroy_staging_buffers(rhi::ResourceManager &rm) {
        if (readback_beauty_.valid()) { rm.destroy_buffer(readback_beauty_); }
        if (readback_albedo_.valid()) { rm.destroy_buffer(readback_albedo_); }
        if (readback_normal_.valid()) { rm.destroy_buffer(readback_normal_); }
        if (upload_.valid()) { rm.destroy_buffer(upload_); }
        readback_beauty_ = {};
        readback_albedo_ = {};
        readback_normal_ = {};
        upload_ = {};
    }
} // namespace himalaya::framework
