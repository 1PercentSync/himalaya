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
    void Denoiser::init(const rhi::Context &ctx, rhi::ResourceManager &rm, const uint32_t width,
                        const uint32_t height) {
        device_ = ctx.device;
        allocator_ = ctx.allocator;
        rm_ = &rm;
        width_ = width;
        height_ = height;

        // ---- OIDN device (GPU preferred, CPU fallback) ----
        const auto oidn_device = new oidn::DeviceRef(oidn::newDevice());
        oidn_device->commit();

        if (const auto device_type = oidn_device->get<oidn::DeviceType>("type");
            device_type == oidn::DeviceType::CPU) {
            spdlog::warn("OIDN: using CPU device (~25x slower than GPU)");
        } else {
            auto type_name = "GPU";
            if (device_type == oidn::DeviceType::CUDA) { type_name = "CUDA GPU"; } else if (
                device_type == oidn::DeviceType::HIP) { type_name = "HIP GPU"; } else if (
                device_type == oidn::DeviceType::SYCL) { type_name = "SYCL GPU"; }
            spdlog::info("OIDN: using {} device", type_name);
        }
        oidn_device_ = oidn_device;

        // ---- OIDN filter ("RT" with albedo + normal auxiliary) ----
        auto *filter = new oidn::FilterRef(oidn_device->newFilter("RT"));
        filter->set("hdr", true);
        filter->set("cleanAux", true);
        filter->set("quality", oidn::Quality::High);
        oidn_filter_ = filter;

        // ---- OIDN buffers (device-managed, host-accessible) ----
        const size_t beauty_size = static_cast<size_t>(width) * height * 16; // RGBA32F
        const size_t aux_size = static_cast<size_t>(width) * height * 8; // RGBA16F

        auto *beauty_buf = new oidn::BufferRef(oidn_device->newBuffer(beauty_size));
        auto *albedo_buf = new oidn::BufferRef(oidn_device->newBuffer(aux_size));
        auto *normal_buf = new oidn::BufferRef(oidn_device->newBuffer(aux_size));
        auto *output_buf = new oidn::BufferRef(oidn_device->newBuffer(beauty_size));

        filter->setImage("color", *beauty_buf, oidn::Format::Float3,
                         width, height, 0, 16, 0);
        filter->setImage("albedo", *albedo_buf, oidn::Format::Half3,
                         width, height, 0, 8, 0);
        filter->setImage("normal", *normal_buf, oidn::Format::Half3,
                         width, height, 0, 8, 0);
        filter->setImage("output", *output_buf, oidn::Format::Float3,
                         width, height, 0, 16, 0);
        filter->commit();

        oidn_beauty_buf_ = beauty_buf;
        oidn_albedo_buf_ = albedo_buf;
        oidn_normal_buf_ = normal_buf;
        oidn_output_buf_ = output_buf;

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

        auto *beauty_buf = static_cast<oidn::BufferRef *>(oidn_beauty_buf_);
        auto *albedo_buf = static_cast<oidn::BufferRef *>(oidn_albedo_buf_);
        auto *normal_buf = static_cast<oidn::BufferRef *>(oidn_normal_buf_);
        const auto *output_buf = static_cast<oidn::BufferRef *>(oidn_output_buf_);
        auto *filter = static_cast<oidn::FilterRef *>(oidn_filter_);
        auto *oidn_dev = static_cast<oidn::DeviceRef *>(oidn_device_);

        // Mapped pointers for Vulkan staging buffers (persistently mapped by VMA)
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

        thread_ = std::jthread([=](const std::stop_token &) {
            // Wait for GPU readback copy to complete
            VkSemaphoreWaitInfo wait_info{};
            wait_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
            wait_info.semaphoreCount = 1;
            wait_info.pSemaphores = &semaphore;
            wait_info.pValues = &wait_value;
            vkWaitSemaphores(vk_device, &wait_info, UINT64_MAX);

            // Invalidate readback buffers for CPU cache coherency.
            // GpuToCpu memory may be HOST_CACHED but not HOST_COHERENT
            // (e.g. AMD discrete GPUs). Without invalidation, CPU may read
            // stale cache lines after GPU writes. No-op on coherent memory.
            vmaInvalidateAllocation(vma, rb_beauty_alloc, 0, VK_WHOLE_SIZE);
            vmaInvalidateAllocation(vma, rb_albedo_alloc, 0, VK_WHOLE_SIZE);
            vmaInvalidateAllocation(vma, rb_normal_alloc, 0, VK_WHOLE_SIZE);

            const size_t beauty_size = static_cast<size_t>(w) * h * 16;
            const size_t aux_size = static_cast<size_t>(w) * h * 8;

            // Copy Vulkan staging → OIDN buffers
            beauty_buf->write(0, beauty_size, readback_beauty_ptr);
            albedo_buf->write(0, aux_size, readback_albedo_ptr);
            normal_buf->write(0, aux_size, readback_normal_ptr);

            // Execute OIDN filter
            filter->execute();
            oidn_dev->sync();

            // Check for errors
            const char *error_msg = nullptr;
            if (const auto error = oidn_dev->getError(error_msg); error != oidn::Error::None) {
                spdlog::error("OIDN filter failed: {} ({})",
                              error_msg ? error_msg : "unknown",
                              static_cast<int>(error));
                state_ptr->store(DenoiseState::Idle, std::memory_order_release);
                return;
            }

            // Copy OIDN output → Vulkan upload staging
            output_buf->read(0, beauty_size, upload_ptr);

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

        // Rebuild OIDN buffers and reconfigure filter for new resolution
        auto &oidn_device = *static_cast<oidn::DeviceRef *>(oidn_device_);
        auto &filter = *static_cast<oidn::FilterRef *>(oidn_filter_);

        const size_t beauty_size = static_cast<size_t>(width) * height * 16;
        const size_t aux_size = static_cast<size_t>(width) * height * 8;

        auto *beauty_buf = new oidn::BufferRef(oidn_device.newBuffer(beauty_size));
        auto *albedo_buf = new oidn::BufferRef(oidn_device.newBuffer(aux_size));
        auto *normal_buf = new oidn::BufferRef(oidn_device.newBuffer(aux_size));
        auto *output_buf = new oidn::BufferRef(oidn_device.newBuffer(beauty_size));

        delete static_cast<oidn::BufferRef *>(oidn_output_buf_);
        delete static_cast<oidn::BufferRef *>(oidn_normal_buf_);
        delete static_cast<oidn::BufferRef *>(oidn_albedo_buf_);
        delete static_cast<oidn::BufferRef *>(oidn_beauty_buf_);

        oidn_beauty_buf_ = beauty_buf;
        oidn_albedo_buf_ = albedo_buf;
        oidn_normal_buf_ = normal_buf;
        oidn_output_buf_ = output_buf;

        filter.setImage("color", *beauty_buf, oidn::Format::Float3,
                        width, height, 0, 16, 0);
        filter.setImage("albedo", *albedo_buf, oidn::Format::Half3,
                        width, height, 0, 8, 0);
        filter.setImage("normal", *normal_buf, oidn::Format::Half3,
                        width, height, 0, 8, 0);
        filter.setImage("output", *output_buf, oidn::Format::Float3,
                        width, height, 0, 16, 0);
        filter.commit();
    }

    void Denoiser::abort() {
        join_and_idle();
    }

    void Denoiser::destroy() {
        join_and_idle();

        // OIDN resources (release in reverse order)
        delete static_cast<oidn::BufferRef *>(oidn_output_buf_);
        delete static_cast<oidn::BufferRef *>(oidn_normal_buf_);
        delete static_cast<oidn::BufferRef *>(oidn_albedo_buf_);
        delete static_cast<oidn::BufferRef *>(oidn_beauty_buf_);
        delete static_cast<oidn::FilterRef *>(oidn_filter_);
        delete static_cast<oidn::DeviceRef *>(oidn_device_);
        oidn_output_buf_ = nullptr;
        oidn_normal_buf_ = nullptr;
        oidn_albedo_buf_ = nullptr;
        oidn_beauty_buf_ = nullptr;
        oidn_filter_ = nullptr;
        oidn_device_ = nullptr;

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

    void Denoiser::join_and_idle() {
        thread_ = {};
        state_.store(DenoiseState::Idle, std::memory_order_release);
    }

    void Denoiser::create_staging_buffers(rhi::ResourceManager &rm,
                                          const uint32_t w, const uint32_t h) {
        const uint64_t beauty_size = static_cast<uint64_t>(w) * h * 16; // RGBA32F
        const uint64_t aux_size = static_cast<uint64_t>(w) * h * 8; // RGBA16F

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
