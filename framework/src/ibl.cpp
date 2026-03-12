/**
 * @file ibl.cpp
 * @brief IBL precomputation implementation.
 *
 * Loads an equirectangular .hdr environment map, uploads it to GPU,
 * and runs four compute shader passes to produce irradiance map,
 * prefiltered environment map, and BRDF integration LUT.
 * All work runs within a single begin_immediate()/end_immediate() scope.
 */

#include <himalaya/framework/ibl.h>

#include <himalaya/rhi/context.h>
#include <himalaya/rhi/descriptors.h>
#include <himalaya/rhi/resources.h>

#include <glm/gtc/packing.hpp>
#include <spdlog/spdlog.h>
#include <stb_image.h>

#include <vector>

namespace himalaya::framework {

    void IBL::init(rhi::Context& ctx, rhi::ResourceManager& rm,
                   rhi::DescriptorManager& dm, rhi::ShaderCompiler& sc,
                   const std::string& hdr_path) {
        ctx_ = &ctx;
        rm_ = &rm;
        dm_ = &dm;

        // ---- 1. Load .hdr file (float32 RGB) ----

        int hdr_width = 0, hdr_height = 0, hdr_channels = 0;
        float* hdr_data = stbi_loadf(hdr_path.c_str(), &hdr_width, &hdr_height, &hdr_channels, 3);
        if (!hdr_data) {
            spdlog::error("IBL: failed to load HDR file: {}", hdr_path);
            return;
        }
        spdlog::info("IBL: loaded {} ({}x{}, {} channels)",
                     hdr_path, hdr_width, hdr_height, hdr_channels);

        // ---- 2. Convert float32 RGB → float16 RGBA ----
        // stbi_loadf returns float32 RGB; GPU equirect uses R16G16B16A16F.
        // CPU-side conversion: pack each component to half-float, add alpha=1.0.

        const auto pixel_count = static_cast<size_t>(hdr_width) * hdr_height;
        std::vector<uint16_t> rgba16(pixel_count * 4);
        for (size_t i = 0; i < pixel_count; ++i) {
            rgba16[i * 4 + 0] = glm::packHalf1x16(hdr_data[i * 3 + 0]);
            rgba16[i * 4 + 1] = glm::packHalf1x16(hdr_data[i * 3 + 1]);
            rgba16[i * 4 + 2] = glm::packHalf1x16(hdr_data[i * 3 + 2]);
            rgba16[i * 4 + 3] = glm::packHalf1x16(1.0f);
        }
        stbi_image_free(hdr_data);

        // ---- 3. Create equirect GPU image + upload ----

        const auto equirect = rm.create_image({
            .width = static_cast<uint32_t>(hdr_width),
            .height = static_cast<uint32_t>(hdr_height),
            .depth = 1,
            .mip_levels = 1,
            .sample_count = 1,
            .format = rhi::Format::R16G16B16A16Sfloat,
            .usage = rhi::ImageUsage::Sampled | rhi::ImageUsage::TransferDst,
        }, "IBL Equirect Input");

        ctx.begin_immediate();
        rm.upload_image(equirect, rgba16.data(), rgba16.size() * sizeof(uint16_t));
        ctx.end_immediate();

        spdlog::info("IBL: equirect uploaded to GPU ({}x{}, R16G16B16A16F)",
                     hdr_width, hdr_height);

        // TODO: Subsequent tasks will insert compute dispatches here
        // (equirect→cubemap, irradiance, prefiltered, BRDF LUT)
        // before destroying the equirect image.

        // Destroy equirect input — no longer needed after cubemap conversion.
        // (Currently destroyed immediately; will be moved after compute dispatches.)
        rm.destroy_image(equirect);
    }

    void IBL::destroy() const {
        // Unregister bindless entries before destroying images
        if (cubemap_bindless_.valid())
            dm_->unregister_cubemap(cubemap_bindless_);
        if (irradiance_bindless_.valid())
            dm_->unregister_cubemap(irradiance_bindless_);
        if (prefiltered_bindless_.valid())
            dm_->unregister_cubemap(prefiltered_bindless_);
        if (brdf_lut_bindless_.valid())
            dm_->unregister_texture(brdf_lut_bindless_);

        // Destroy images
        if (cubemap_.valid())
            rm_->destroy_image(cubemap_);
        if (irradiance_.valid())
            rm_->destroy_image(irradiance_);
        if (prefiltered_.valid())
            rm_->destroy_image(prefiltered_);
        if (brdf_lut_.valid())
            rm_->destroy_image(brdf_lut_);

        // Destroy sampler
        if (sampler_.valid())
            rm_->destroy_sampler(sampler_);
    }

    rhi::BindlessIndex IBL::irradiance_cubemap_index() const {
        return irradiance_bindless_;
    }

    rhi::BindlessIndex IBL::prefiltered_cubemap_index() const {
        return prefiltered_bindless_;
    }

    rhi::BindlessIndex IBL::brdf_lut_index() const {
        return brdf_lut_bindless_;
    }

    rhi::BindlessIndex IBL::skybox_cubemap_index() const {
        return cubemap_bindless_;
    }

    uint32_t IBL::prefiltered_mip_count() const {
        return prefiltered_mip_count_;
    }

} // namespace himalaya::framework
