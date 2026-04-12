#pragma once

/**
 * @file material_system.h
 * @brief Material system: GPU material data layout, material instances, and global Material SSBO.
 */

#include <himalaya/rhi/types.h>

#include <glm/glm.hpp>

#include <span>

namespace himalaya::rhi {
    class DescriptorManager;
    class ResourceManager;
}

namespace himalaya::framework {
    /**
     * @brief Alpha blending mode (matches glTF alphaMode).
     *
     * Determines both CPU-side draw call routing (which pass) and
     * GPU-side shader behavior (discard for Mask).
     */
    enum class AlphaMode : uint32_t {
        Opaque = 0, ///< Fully opaque, alpha ignored
        Mask = 1, ///< Alpha test: discard if alpha < alpha_cutoff
        Blend = 2, ///< Alpha blending (transparent pass, back-to-front)
    };

    /**
     * @brief GPU-side material data (Set 0, Binding 2 SSBO element).
     *
     * std430 layout, 80 bytes per element, aligned to 16.
     * Texture fields hold bindless indices into the global texture array (Set 1).
     * Must match the shader-side GPUMaterialData in shaders/common/bindings.glsl.
     */
    struct alignas(16) GPUMaterialData {
        glm::vec4 base_color_factor; ///< offset  0 — glTF baseColorFactor (RGBA)
        glm::vec4 emissive_factor; ///< offset 16 — xyz = emissiveFactor, w unused

        float metallic_factor; ///< offset 32 — glTF metallicFactor
        float roughness_factor; ///< offset 36 — glTF roughnessFactor
        float normal_scale; ///< offset 40 — glTF normalTexture.scale
        float occlusion_strength; ///< offset 44 — glTF occlusionTexture.strength

        uint32_t base_color_tex; ///< offset 48 — bindless index
        uint32_t emissive_tex; ///< offset 52 — bindless index
        uint32_t metallic_roughness_tex; ///< offset 56 — bindless index
        uint32_t normal_tex; ///< offset 60 — bindless index

        uint32_t occlusion_tex; ///< offset 64 — bindless index
        float alpha_cutoff; ///< offset 68 — glTF alphaCutoff (Mask mode threshold)
        uint32_t alpha_mode; ///< offset 72 — AlphaMode as uint (0/1/2)
        uint32_t double_sided; ///< offset 76 — 1 if glTF doubleSided, 0 otherwise
    };

    static_assert(sizeof(GPUMaterialData) == 80, "GPUMaterialData must be 80 bytes (std430)");

    /**
     * @brief Per-material instance metadata.
     *
     * Links a material to its shading model (template) and its position
     * in the global Material SSBO. In M1 only standard PBR exists,
     * so template_id is always 0.
     */
    struct MaterialInstance {
        /** @brief Shading model identifier (0 = standard PBR). */
        uint32_t template_id;

        /** @brief Index into the GPUMaterialData array in the Material SSBO. */
        uint32_t buffer_offset;

        /** @brief Alpha mode — determines pass routing (opaque / mask / transparent). */
        AlphaMode alpha_mode;

        /** @brief Whether to disable back-face culling for this material. */
        bool double_sided;
    };

    /**
     * @brief Fills unset texture fields (UINT32_MAX) with default bindless indices.
     *
     * Mapping:
     * - base_color_tex, metallic_roughness_tex, occlusion_tex → default_white (neutral)
     * - normal_tex → default_flat_normal (no perturbation)
     * - emissive_tex → default_black (no emission)
     *
     * @param data               Material data to patch.
     * @param default_white      Bindless index of the 1x1 white texture.
     * @param default_flat_normal Bindless index of the 1x1 flat normal texture.
     * @param default_black      Bindless index of the 1x1 black texture.
     */
    void fill_material_defaults(GPUMaterialData &data,
                                rhi::BindlessIndex default_white,
                                rhi::BindlessIndex default_flat_normal,
                                rhi::BindlessIndex default_black);

    /**
     * @brief Manages the global Material SSBO (Set 0, Binding 2).
     *
     * Holds all material data in a single GPU buffer. Scene loader creates
     * GPUMaterialData entries, then MaterialSystem uploads them and writes
     * the descriptor to both per-frame Set 0 instances.
     *
     * The SSBO is sized exactly to the loaded material count (no over-allocation).
     * Supports scene switch: calling upload_materials() again destroys the
     * previous buffer before creating a new one.
     *
     * Lifetime: init() → upload_materials() (once per scene load) → destroy().
     */
    class MaterialSystem {
    public:
        /**
         * @brief Stores RHI references for later use.
         *
         * Does not create any GPU resources; those are created in upload_materials().
         */
        void init(rhi::ResourceManager *resource_manager, rhi::DescriptorManager *descriptor_manager);

        /**
         * @brief Destroys the Material SSBO.
         *
         * Safe to call even if upload_materials() was never called.
         */
        void destroy();

        /**
         * @brief Creates the SSBO, uploads material data, and writes the descriptor.
         *
         * Must be called within a Context::begin_immediate() / end_immediate() scope.
         * The SSBO uses GpuOnly memory; data is uploaded via staging buffer.
         * Writes the MaterialBuffer descriptor (binding 2) to both per-frame Set 0.
         *
         * @param materials Array of GPU material data to upload.
         */
        void upload_materials(std::span<const GPUMaterialData> materials);

        /** @brief Returns the Material SSBO buffer handle. */
        [[nodiscard]] rhi::BufferHandle get_buffer() const;

        /** @brief Returns the number of uploaded materials. */
        [[nodiscard]] uint32_t material_count() const;

    private:
        /** @brief Resource manager for buffer creation and upload. */
        rhi::ResourceManager *resource_manager_ = nullptr;

        /** @brief Descriptor manager for descriptor writes. */
        rhi::DescriptorManager *descriptor_manager_ = nullptr;

        /** @brief GPU buffer holding the material data array. */
        rhi::BufferHandle material_buffer_;

        /** @brief Number of materials in the SSBO. */
        uint32_t material_count_ = 0;
    };
} // namespace himalaya::framework
