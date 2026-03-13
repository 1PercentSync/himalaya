#pragma once

/**
 * @file ibl.h
 * @brief Image-Based Lighting precomputation module.
 */

#include <himalaya/rhi/types.h>
#include <string>

namespace himalaya::rhi {
    class Context;
    class ResourceManager;
    class DescriptorManager;
    class ShaderCompiler;
}

namespace himalaya::framework {

    /**
     * @brief Manages IBL precomputation: equirect HDR → cubemap → irradiance / prefiltered / BRDF LUT.
     *
     * Owns all GPU resources created during init (cubemap, irradiance, prefiltered, BRDF LUT).
     * Registers precomputed products to Set 1 bindless arrays for shader access.
     * The equirect input image is loaded, converted, and destroyed within init() — it does not
     * outlive the init scope.
     *
     * Typical usage:
     * @code
     *   IBL ibl;
     *   // Inside begin_immediate() / end_immediate() scope:
     *   ibl.init(ctx, rm, dm, sc, "assets/environment.hdr");
     *   // ... use bindless indices in GlobalUBO ...
     *   ibl.destroy();
     * @endcode
     */
    class IBL {
    public:
        /**
         * @brief Load an .hdr file and run the full IBL precomputation pipeline.
         *
         * Performs all work inside a begin_immediate() / end_immediate() scope
         * (caller must set up the scope). Pipeline stages:
         * 1. stbi_loadf → equirect R16G16B16A16F GPU image
         * 2. Equirect → cubemap (1024×1024 per face, compute shader)
         * 3. Irradiance convolution (32×32 per face, R11G11B10F, compute)
         * 4. Prefiltered environment map (256×256 per face, multi-mip, R16G16B16A16F, compute)
         * 5. BRDF Integration LUT (256×256, R16G16_UNORM, compute)
         *
         * Products are registered to Set 1 bindless arrays. The equirect input image
         * is destroyed before init() returns.
         *
         * @param ctx      RHI context (device, queue, immediate command scope).
         * @param rm       Resource manager for image creation and upload.
         * @param dm       Descriptor manager for bindless cubemap/texture registration.
         * @param sc       Shader compiler for compute shader compilation.
         * @param hdr_path Filesystem path to an equirectangular .hdr environment map.
         */
        void init(rhi::Context& ctx, rhi::ResourceManager& rm,
                  rhi::DescriptorManager& dm, rhi::ShaderCompiler& sc,
                  const std::string& hdr_path);

        /**
         * @brief Unregister bindless entries and destroy all owned GPU resources.
         *
         * Unregisters cubemap/texture bindless slots first, then destroys the
         * underlying images. Safe to call even if init() was never called
         * (handles default to invalid).
         */
        void destroy();

        /** @brief Bindless index of the irradiance cubemap in Set 1 cubemaps[]. */
        [[nodiscard]] rhi::BindlessIndex irradiance_cubemap_index() const;

        /** @brief Bindless index of the prefiltered environment cubemap in Set 1 cubemaps[]. */
        [[nodiscard]] rhi::BindlessIndex prefiltered_cubemap_index() const;

        /** @brief Bindless index of the BRDF integration LUT in Set 1 textures[]. */
        [[nodiscard]] rhi::BindlessIndex brdf_lut_index() const;

        /** @brief Bindless index of the intermediate cubemap for Skybox Pass in Set 1 cubemaps[]. */
        [[nodiscard]] rhi::BindlessIndex skybox_cubemap_index() const;

        /** @brief Number of mip levels in the prefiltered environment map (for roughness → mip mapping). */
        [[nodiscard]] uint32_t prefiltered_mip_count() const;

    private:
        // --- Service pointers (stored for destroy) ---
        rhi::ResourceManager* rm_ = nullptr;
        rhi::DescriptorManager* dm_ = nullptr;

        // --- GPU resources (owned) ---
        rhi::ImageHandle cubemap_;             ///< Intermediate cubemap (1024×1024, kept for Skybox)
        rhi::ImageHandle irradiance_cubemap_;  ///< Irradiance map (32×32 per face)
        rhi::ImageHandle prefiltered_cubemap_; ///< Prefiltered env map (256×256, multi-mip)
        rhi::ImageHandle brdf_lut_;            ///< BRDF integration LUT (256×256)

        // --- Shared sampler for all IBL products ---
        rhi::SamplerHandle sampler_;

        // --- Bindless indices (Set 1 registration) ---
        rhi::BindlessIndex skybox_cubemap_idx_;
        rhi::BindlessIndex irradiance_cubemap_idx_;
        rhi::BindlessIndex prefiltered_cubemap_idx_;
        rhi::BindlessIndex brdf_lut_idx_;

        // --- Prefiltered mip count ---
        uint32_t prefiltered_mip_count_ = 0;
    };

} // namespace himalaya::framework
