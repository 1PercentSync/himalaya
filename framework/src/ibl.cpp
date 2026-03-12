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

#include <spdlog/spdlog.h>

namespace himalaya::framework {

    void IBL::init(rhi::Context& ctx, rhi::ResourceManager& rm,
                   rhi::DescriptorManager& dm, rhi::ShaderCompiler& sc,
                   const std::string& hdr_path) {
        ctx_ = &ctx;
        rm_ = &rm;
        dm_ = &dm;

        // TODO: Implement in subsequent tasks:
        // 1. Load .hdr with stbi_loadf, upload to equirect GPU image
        // 2. Create cubemap, irradiance, prefiltered, BRDF LUT images
        // 3. Create clamp-to-edge sampler
        // 4. Compile compute shaders, create pipelines
        // 5. begin_immediate() scope:
        //    - equirect → cubemap dispatch
        //    - cubemap → irradiance dispatch
        //    - cubemap → prefiltered dispatch (per mip level)
        //    - BRDF LUT dispatch
        // 6. Destroy equirect input image (no longer needed)
        // 7. Register products into bindless arrays

        spdlog::info("IBL: module skeleton initialized (hdr_path={})", hdr_path);
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
