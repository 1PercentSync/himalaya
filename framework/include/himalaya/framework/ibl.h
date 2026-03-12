#pragma once

/**
 * @file ibl.h
 * @brief IBL precomputation module (framework layer).
 *
 * Loads an equirectangular .hdr environment map and runs GPU compute
 * precomputation: equirect → cubemap → irradiance / prefiltered / BRDF LUT.
 * Products are registered into the bindless arrays (Set 1) for shader
 * consumption. The module self-manages all image resources.
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
     * @brief Image-Based Lighting precomputation module.
     *
     * Self-manages all IBL resources: intermediate cubemap, irradiance map,
     * prefiltered environment map, and BRDF integration LUT.
     *
     * init() loads an .hdr file and runs the full precomputation pipeline
     * within a single begin_immediate()/end_immediate() scope. Each compute
     * dispatch uses push descriptors (project exception for one-time init).
     *
     * destroy() unregisters bindless entries before destroying images.
     */
    class IBL {
    public:
        /**
         * @brief Loads .hdr and executes all IBL precomputation.
         *
         * Pipeline: stbi_loadf → GPU upload (R16G16B16A16F equirect) →
         * equirect→cubemap (1024²/face) → irradiance convolution (32²/face) →
         * prefiltered env map (256²/face, mipmapped) → BRDF LUT (256²).
         * All dispatches run in begin_immediate()/end_immediate() scope.
         *
         * @param ctx      Vulkan context (device, queues, immediate scope).
         * @param rm       Resource manager (image creation, upload).
         * @param dm       Descriptor manager (bindless registration).
         * @param sc       Shader compiler (compute shader compilation).
         * @param hdr_path Path to the .hdr environment map file.
         */
        void init(rhi::Context &ctx,
                  rhi::ResourceManager &rm,
                  rhi::DescriptorManager &dm,
                  rhi::ShaderCompiler &sc,
                  const std::string &hdr_path);

        /**
         * @brief Unregisters bindless entries and destroys all IBL images.
         *
         * Order: unregister cubemaps/textures → destroy images → destroy sampler.
         * The equirectangular input image is already destroyed at end of init().
         */
        void destroy() const;

        /** @brief Irradiance cubemap bindless index (cubemaps[] array). */
        [[nodiscard]] rhi::BindlessIndex irradiance_cubemap_index() const;

        /** @brief Prefiltered environment cubemap bindless index (cubemaps[] array). */
        [[nodiscard]] rhi::BindlessIndex prefiltered_cubemap_index() const;

        /** @brief BRDF integration LUT bindless index (textures[] array). */
        [[nodiscard]] rhi::BindlessIndex brdf_lut_index() const;

        /** @brief Intermediate cubemap bindless index for Skybox Pass (cubemaps[] array). */
        [[nodiscard]] rhi::BindlessIndex skybox_cubemap_index() const;

        /** @brief Number of mip levels in the prefiltered environment map. */
        [[nodiscard]] uint32_t prefiltered_mip_count() const;

    private:
        // --- Subsystem references (set during init, non-owning) ---
        rhi::Context *ctx_ = nullptr;
        rhi::ResourceManager *rm_ = nullptr;
        rhi::DescriptorManager *dm_ = nullptr;

        // --- IBL images (self-managed, destroyed in destroy()) ---

        /** @brief Intermediate cubemap (1024×1024/face, R16G16B16A16F). Kept for Skybox. */
        rhi::ImageHandle cubemap_;

        /** @brief Irradiance cubemap (32×32/face, R11G11B10F). */
        rhi::ImageHandle irradiance_;

        /** @brief Prefiltered environment cubemap (256×256/face, R16G16B16A16F, mipmapped). */
        rhi::ImageHandle prefiltered_;

        /** @brief BRDF integration LUT (256×256, R16G16_UNORM). */
        rhi::ImageHandle brdf_lut_;

        /** @brief Clamp-to-edge sampler shared by all IBL products. */
        rhi::SamplerHandle sampler_;

        // --- Bindless indices (registered during init, unregistered in destroy) ---
        rhi::BindlessIndex cubemap_bindless_; ///< Skybox cubemap
        rhi::BindlessIndex irradiance_bindless_; ///< Irradiance cubemap
        rhi::BindlessIndex prefiltered_bindless_; ///< Prefiltered cubemap
        rhi::BindlessIndex brdf_lut_bindless_; ///< BRDF LUT

        /** @brief Number of mip levels in prefiltered map (roughness → mip mapping). */
        uint32_t prefiltered_mip_count_ = 0;
    };
} // namespace himalaya::framework
