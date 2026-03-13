#pragma once

/**
 * @file ibl.h
 * @brief Image-Based Lighting precomputation module.
 */

#include <himalaya/rhi/types.h>
#include <functional>
#include <string>
#include <vector>

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
         * All GPU work is recorded into a single begin_immediate() / end_immediate()
         * scope. Transient resources are destroyed after GPU completion.
         * Pipeline stages:
         * 1. stbi_loadf → equirect R16G16B16A16F GPU image
         * 2. Equirect → cubemap (face size derived from input width, compute shader)
         * 3. Irradiance convolution (32×32 per face, R11G11B10F, compute)
         * 4. Prefiltered environment map (512×512 per face, multi-mip, R16G16B16A16F, compute)
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
        void init(rhi::Context &ctx,
                  rhi::ResourceManager &rm,
                  rhi::DescriptorManager &dm,
                  rhi::ShaderCompiler &sc,
                  const std::string &hdr_path);

        /**
         * @brief Unregister bindless entries and destroy all owned GPU resources.
         *
         * Unregisters cubemap/texture bindless slots first, then destroys the
         * underlying images. Safe to call even if init() was never called
         * (handles default to invalid).
         */
        void destroy() const;

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
        /** @brief Deferred cleanup function list, executed after end_immediate(). */
        using DeferredCleanup = std::vector<std::function<void()> >;

        /** @brief Return value of load_equirect(). */
        struct EquirectResult { // NOLINT(*-pro-type-member-init)
            rhi::ImageHandle image; ///< GPU image handle (SHADER_READ_ONLY layout).
            uint32_t width; ///< Equirect image width in pixels.
        };

        /**
         * @brief Load .hdr file and upload as equirectangular GPU image.
         *
         * stbi_loadf → RGB→RGBA expansion → float32→float16 conversion →
         * create R16G16B16A16F 2D image → upload via staging buffer.
         * Must be called within an active immediate scope.
         *
         * @return Equirect image handle and width. Caller must destroy the image.
         *         Returns invalid handle (valid() == false) if loading fails.
         */
        [[nodiscard]] EquirectResult load_equirect(const std::string &hdr_path) const;

        /**
         * @brief Create minimal 1×1 neutral gray cubemaps as fallback when HDR loading fails.
         *
         * Uses vkCmdClearColorImage to fill cubemap_, irradiance_cubemap_, and
         * prefiltered_cubemap_ with a uniform neutral gray. The pipeline (bindless
         * registration, skybox, forward IBL) works identically — no shader-side
         * conditionals needed.
         * Must be called within an active immediate scope.
         *
         * @param ctx RHI context (device, immediate command buffer).
         */
        void create_fallback_cubemaps(const rhi::Context &ctx);

        /**
         * @brief Convert equirectangular image to a cubemap via compute shader.
         *
         * Cubemap face size is derived from the equirect width to match angular
         * resolution: min(bit_ceil(equirect_width / 4), 2048).
         * Dispatches equirect_to_cubemap.comp using push descriptors, and
         * transitions the cubemap to SHADER_READ_ONLY for subsequent sampling.
         * Must be called within an active immediate scope.
         *
         * Transient resources (pipeline, image views, sampler) are pushed to
         * @p deferred for destruction after end_immediate().
         *
         * @param ctx            RHI context (device, immediate command buffer).
         * @param sc             Shader compiler for compute shader compilation.
         * @param equirect       Equirect image handle (must be in SHADER_READ_ONLY layout).
         * @param equirect_width Equirect image width in pixels (used to compute cubemap face size).
         * @param deferred       Cleanup functions executed after GPU completion.
         */
        void convert_equirect_to_cubemap(rhi::Context &ctx,
                                         rhi::ShaderCompiler &sc,
                                         rhi::ImageHandle equirect,
                                         uint32_t equirect_width,
                                         DeferredCleanup &deferred);

        /**
         * @brief Compute diffuse irradiance cubemap via cosine-weighted hemisphere convolution.
         *
         * Samples the intermediate cubemap (cubemap_) over the hemisphere for each output
         * texel, producing a low-resolution irradiance map (32x32 per face, R11G11B10F).
         * Must be called within an active immediate scope, after convert_equirect_to_cubemap().
         *
         * Transient resources (pipeline, image views, sampler) are pushed to
         * @p deferred for destruction after end_immediate().
         *
         * @param ctx      RHI context (device, immediate command buffer).
         * @param sc       Shader compiler for compute shader compilation.
         * @param deferred Cleanup functions executed after GPU completion.
         */
        void compute_irradiance(rhi::Context &ctx,
                                rhi::ShaderCompiler &sc,
                                DeferredCleanup &deferred);

        /**
         * @brief Generate prefiltered environment cubemap for specular IBL.
         *
         * Creates a 512x512 cubemap with a full mip chain, where each mip level
         * stores the environment convolved at increasing roughness. Uses GGX
         * importance sampling with 1024 samples per texel. Roughness is passed
         * via push constant, one dispatch per mip level.
         * Must be called within an active immediate scope, after convert_equirect_to_cubemap().
         *
         * Transient resources (pipeline, per-mip image views, sampler) are pushed to
         * @p deferred for destruction after end_immediate().
         *
         * @param ctx      RHI context (device, immediate command buffer).
         * @param sc       Shader compiler for compute shader compilation.
         * @param deferred Cleanup functions executed after GPU completion.
         */
        void compute_prefiltered(rhi::Context &ctx,
                                 rhi::ShaderCompiler &sc,
                                 DeferredCleanup &deferred);

        /**
         * @brief Compute the BRDF integration lookup table for Split-Sum IBL.
         *
         * Creates a 256x256 R16G16_UNORM 2D image. For each (NdotV, roughness) pair,
         * integrates the GGX BRDF to produce (scale, bias) for the Fresnel term.
         * Environment-independent — shared across all IBL environments.
         * Must be called within an active immediate scope.
         *
         * Transient resources (pipeline) are pushed to @p deferred for destruction
         * after end_immediate().
         *
         * @param ctx      RHI context (device, immediate command buffer).
         * @param sc       Shader compiler for compute shader compilation.
         * @param deferred Cleanup functions executed after GPU completion.
         */
        void compute_brdf_lut(rhi::Context &ctx,
                              rhi::ShaderCompiler &sc,
                              DeferredCleanup &deferred);

        /**
         * @brief Create shared sampler and register precomputed products to Set 1 bindless.
         *
         * Registers cubemaps (skybox, irradiance, prefiltered) to Set 1 binding 1
         * and BRDF LUT to Set 1 binding 0. Must be called after all compute
         * stages complete and end_immediate() returns.
         */
        void register_bindless_resources();

        // --- Service pointers (stored for destroy) ---
        rhi::ResourceManager *rm_ = nullptr;
        rhi::DescriptorManager *dm_ = nullptr;

        // --- GPU resources (owned) ---
        rhi::ImageHandle cubemap_; ///< Intermediate cubemap (size derived from input, kept for Skybox)
        rhi::ImageHandle irradiance_cubemap_; ///< Irradiance map (32×32 per face)
        rhi::ImageHandle prefiltered_cubemap_; ///< Prefiltered env map (512×512, multi-mip)
        rhi::ImageHandle brdf_lut_; ///< BRDF integration LUT (256×256)

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
