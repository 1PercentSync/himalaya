/**
 * @file bake_denoiser.cpp
 * @brief Synchronous OIDN denoiser for offline baking.
 */

#include <himalaya/framework/bake_denoiser.h>

#include <OpenImageDenoise/oidn.hpp>
#include <spdlog/spdlog.h>

namespace himalaya::framework {

    constexpr size_t kBeautyBpp = 16; // RGBA32F
    constexpr size_t kAuxBpp = 8;    // RGBA16F

    /** @brief PIMPL holding all OIDN objects, hidden from the header. */
    struct BakeDenoiser::OidnImpl {
        oidn::DeviceRef device;
        oidn::FilterRef filter;
        oidn::BufferRef beauty_buf;
        oidn::BufferRef albedo_buf;
        oidn::BufferRef normal_buf;
        oidn::BufferRef output_buf;
    };

    BakeDenoiser::BakeDenoiser() = default;

    BakeDenoiser::~BakeDenoiser() = default;

    void BakeDenoiser::init() {
        oidn_ = std::make_unique<OidnImpl>();

        // ---- OIDN device (GPU preferred, CPU fallback) ----
        oidn_->device = oidn::newDevice();
        oidn_->device.commit();

        if (const auto device_type = oidn_->device.get<oidn::DeviceType>("type");
            device_type == oidn::DeviceType::CPU) {
            spdlog::warn("BakeDenoiser: using CPU device (~25x slower than GPU)");
        } else {
            auto type_name = "GPU";
            if (device_type == oidn::DeviceType::CUDA) {
                type_name = "CUDA GPU";
            } else if (device_type == oidn::DeviceType::HIP) {
                type_name = "HIP GPU";
            } else if (device_type == oidn::DeviceType::SYCL) {
                type_name = "SYCL GPU";
            }
            spdlog::info("BakeDenoiser: using {} device", type_name);
        }

        // ---- OIDN filter ("RT" with albedo + normal auxiliary) ----
        oidn_->filter = oidn_->device.newFilter("RT");
        oidn_->filter.set("hdr", true);
        oidn_->filter.set("cleanAux", true);
        oidn_->filter.set("quality", oidn::Quality::High);
    }

    void BakeDenoiser::ensure_buffers(const uint32_t w, const uint32_t h) {
        if (buf_width_ == w && buf_height_ == h) {
            return;
        }

        const size_t beauty_size = static_cast<size_t>(w) * h * kBeautyBpp;
        const size_t aux_size = static_cast<size_t>(w) * h * kAuxBpp;

        // Assignment to BufferRef releases the old buffer via reference counting.
        oidn_->beauty_buf = oidn_->device.newBuffer(beauty_size);
        oidn_->albedo_buf = oidn_->device.newBuffer(aux_size);
        oidn_->normal_buf = oidn_->device.newBuffer(aux_size);
        oidn_->output_buf = oidn_->device.newBuffer(beauty_size);

        oidn_->filter.setImage("color", oidn_->beauty_buf, oidn::Format::Float3,
                               w, h, 0, kBeautyBpp, 0);
        oidn_->filter.setImage("albedo", oidn_->albedo_buf, oidn::Format::Half3,
                               w, h, 0, kAuxBpp, 0);
        oidn_->filter.setImage("normal", oidn_->normal_buf, oidn::Format::Half3,
                               w, h, 0, kAuxBpp, 0);
        oidn_->filter.setImage("output", oidn_->output_buf, oidn::Format::Float3,
                               w, h, 0, kBeautyBpp, 0);
        oidn_->filter.commit();

        buf_width_ = w;
        buf_height_ = h;

        spdlog::info("BakeDenoiser: allocated buffers {}x{}", w, h);
    }

    bool BakeDenoiser::denoise(const void *beauty, const void *albedo, const void *normal,
                               void *output, const uint32_t w, const uint32_t h) {
        ensure_buffers(w, h);

        const size_t beauty_size = static_cast<size_t>(w) * h * kBeautyBpp;
        const size_t aux_size = static_cast<size_t>(w) * h * kAuxBpp;

        // Copy input data → OIDN buffers
        oidn_->beauty_buf.write(0, beauty_size, beauty);
        if (albedo) {
            oidn_->albedo_buf.write(0, aux_size, albedo);
        }
        if (normal) {
            oidn_->normal_buf.write(0, aux_size, normal);
        }

        // Execute filter (blocking)
        oidn_->filter.execute();
        oidn_->device.sync();

        // Error check
        const char *error_msg = nullptr;
        if (const auto error = oidn_->device.getError(error_msg); error != oidn::Error::None) {
            spdlog::error("BakeDenoiser: filter failed: {} ({})",
                          error_msg ? error_msg : "unknown",
                          static_cast<int>(error));
            return false;
        }

        // Copy OIDN output → caller buffer
        oidn_->output_buf.read(0, beauty_size, output);
        return true;
    }

    void BakeDenoiser::destroy() {
        oidn_.reset();
        buf_width_ = 0;
        buf_height_ = 0;
    }

} // namespace himalaya::framework
