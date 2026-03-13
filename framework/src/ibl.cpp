/**
 * @file ibl.cpp
 * @brief IBL precomputation implementation.
 *
 * Loads an equirectangular .hdr environment map, converts it to a cubemap on the GPU,
 * then generates irradiance, prefiltered environment, and BRDF integration LUT via
 * compute shaders. All work executes inside a begin_immediate() / end_immediate() scope
 * with manual pipeline barriers between stages.
 */

#include <himalaya/framework/ibl.h>
#include <himalaya/rhi/context.h>
#include <himalaya/rhi/resources.h>
#include <himalaya/rhi/descriptors.h>
#include <himalaya/rhi/shader.h>

#include <glm/gtc/packing.hpp>
#include <spdlog/spdlog.h>
#include <stb_image.h>

#include <cassert>
#include <vector>

namespace himalaya::framework {
    void IBL::init(rhi::Context &ctx,
                   rhi::ResourceManager &rm,
                   rhi::DescriptorManager &dm,
                   rhi::ShaderCompiler &sc,
                   const std::string &hdr_path) {
        rm_ = &rm;
        dm_ = &dm;

        // TODO: Implement IBL precomputation pipeline
        // 1. auto equirect = load_equirect(hdr_path);
        // 2. convert_equirect_to_cubemap(ctx, sc, equirect);
        // 3. rm_->destroy_image(equirect);
        // 4. compute_irradiance(ctx, sc);
        // 5. compute_prefiltered(ctx, sc);
        // 6. compute_brdf_lut(ctx, sc);
        // 7. register_bindless_resources();
    }

    // -----------------------------------------------------------------------
    // load_equirect — Load .hdr file, convert RGB f32 → RGBA f16, upload GPU
    // -----------------------------------------------------------------------

    rhi::ImageHandle IBL::load_equirect(const std::string &hdr_path) const {
        // Load HDR file as RGB float32
        int w, h, channels;
        float *rgb_data = stbi_loadf(hdr_path.c_str(), &w, &h, &channels, 3);
        if (!rgb_data) {
            spdlog::error("Failed to load HDR environment map '{}': {}",
                          hdr_path, stbi_failure_reason());
            return {};
        }
        spdlog::info("Loaded HDR environment map '{}' ({}x{})", hdr_path, w, h);

        // Convert RGB float32 → RGBA float16
        const auto pixel_count = static_cast<uint32_t>(w) * static_cast<uint32_t>(h);
        std::vector<uint16_t> rgba16(pixel_count * 4);

        for (uint32_t i = 0; i < pixel_count; ++i) {
            rgba16[i * 4 + 0] = glm::packHalf1x16(rgb_data[i * 3 + 0]);
            rgba16[i * 4 + 1] = glm::packHalf1x16(rgb_data[i * 3 + 1]);
            rgba16[i * 4 + 2] = glm::packHalf1x16(rgb_data[i * 3 + 2]);
            rgba16[i * 4 + 3] = glm::packHalf1x16(1.0f);
        }

        stbi_image_free(rgb_data);

        // Create GPU image and upload
        const rhi::ImageDesc desc{
            .width = static_cast<uint32_t>(w),
            .height = static_cast<uint32_t>(h),
            .depth = 1,
            .mip_levels = 1,
            .sample_count = 1,
            .format = rhi::Format::R16G16B16A16Sfloat,
            .usage = rhi::ImageUsage::Sampled | rhi::ImageUsage::TransferDst,
        };

        const auto image = rm_->create_image(desc, "IBL Equirect");
        rm_->upload_image(image, rgba16.data(), rgba16.size() * sizeof(uint16_t));

        return image;
    }

    void IBL::destroy() {
        if (!rm_) return;

        // TODO: Unregister bindless entries, then destroy images and sampler
    }

    rhi::BindlessIndex IBL::irradiance_cubemap_index() const {
        return irradiance_cubemap_idx_;
    }

    rhi::BindlessIndex IBL::prefiltered_cubemap_index() const {
        return prefiltered_cubemap_idx_;
    }

    rhi::BindlessIndex IBL::brdf_lut_index() const {
        return brdf_lut_idx_;
    }

    rhi::BindlessIndex IBL::skybox_cubemap_index() const {
        return skybox_cubemap_idx_;
    }

    uint32_t IBL::prefiltered_mip_count() const {
        return prefiltered_mip_count_;
    }
} // namespace himalaya::framework
