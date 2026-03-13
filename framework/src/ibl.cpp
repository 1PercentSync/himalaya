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

namespace himalaya::framework {

    void IBL::init(rhi::Context& ctx, rhi::ResourceManager& rm,
                   rhi::DescriptorManager& dm, rhi::ShaderCompiler& sc,
                   const std::string& hdr_path) {
        rm_ = &rm;
        dm_ = &dm;

        // TODO: Implement IBL precomputation pipeline
        // 1. Load .hdr file with stbi_loadf
        // 2. Upload to equirect GPU image (R16G16B16A16F)
        // 3. Equirect → cubemap (compute shader)
        // 4. Irradiance convolution (compute shader)
        // 5. Prefiltered environment map (compute shader, multi-mip)
        // 6. BRDF integration LUT (compute shader)
        // 7. Register products to Set 1 bindless arrays
        // 8. Destroy equirect input image
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
