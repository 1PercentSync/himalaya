#pragma once

/**
 * @file texture.h
 * @brief Texture loading, GPU upload, mip generation, and bindless registration.
 */

#include <himalaya/rhi/types.h>
#include <memory>
#include <string>

namespace himalaya::rhi {
    class DescriptorManager;
    class ResourceManager;
}

namespace himalaya::framework {
    /**
     * @brief Determines the GPU format for a texture based on its role.
     *
     * Color data (base color, emissive) uses SRGB for correct gamma.
     * Linear data (normal, roughness, occlusion) uses UNORM for raw values.
     */
    enum class TextureRole {
        Color, ///< R8G8B8A8_SRGB (gamma-correct color data)
        Linear, ///< R8G8B8A8_UNORM (linear data: normals, roughness, etc.)
    };

    /**
     * @brief CPU-side image pixel data loaded from disk.
     *
     * Owns the pixel buffer via a custom deleter that calls stbi_image_free.
     * Always RGBA8 (4 bytes per pixel) regardless of source channel count.
     */
    struct ImageData {
        /** @brief RGBA8 pixel buffer. */
        std::unique_ptr<uint8_t[], void(*)(void *)> pixels{nullptr, nullptr};

        uint32_t width = 0;
        uint32_t height = 0;

        /** @brief Total size in bytes (width * height * 4). */
        [[nodiscard]] uint64_t size_bytes() const { return static_cast<uint64_t>(width) * height * 4; }

        [[nodiscard]] bool valid() const { return pixels != nullptr; }
    };

    /**
     * @brief Loads an image file (JPEG/PNG) into CPU memory as RGBA8.
     *
     * Forces 4 channels regardless of source format. Returns invalid
     * ImageData on failure (check with valid()).
     */
    [[nodiscard]] ImageData load_image(const std::string &path);

    /**
     * @brief Loads an image from an in-memory buffer as RGBA8.
     *
     * Decodes JPEG/PNG/etc. from raw bytes using stb_image. Forces 4
     * channels regardless of source format. Returns invalid ImageData
     * on failure (check with valid()).
     *
     * @param buffer      Pointer to the encoded image data.
     * @param byte_length Size of the buffer in bytes.
     */
    [[nodiscard]] ImageData load_image_from_memory(const uint8_t *buffer, size_t byte_length);

    /**
     * @brief Result of creating and registering a texture.
     */
    struct TextureResult {
        rhi::ImageHandle image;
        rhi::BindlessIndex bindless_index;
    };

    /**
     * @brief Creates a GPU texture from CPU pixel data and registers it to bindless.
     *
     * Must be called within a Context::begin_immediate() / end_immediate() scope.
     * Full pipeline: create image → upload via staging → generate mips → register
     * to bindless array. The image ends in SHADER_READ_ONLY layout.
     *
     * @param resource_manager   RHI resource manager for image/buffer operations.
     * @param descriptor_manager Descriptor manager for bindless registration.
     * @param data               CPU pixel data (must be valid).
     * @param role               Texture role determining the GPU format.
     * @param sampler            Sampler to pair with the texture.
     * @param debug_name         Human-readable name for the GPU image (must not be null).
     * @return Image handle and bindless index.
     */
    [[nodiscard]] TextureResult create_texture(rhi::ResourceManager &resource_manager,
                                               rhi::DescriptorManager &descriptor_manager,
                                               const ImageData &data,
                                               TextureRole role,
                                               rhi::SamplerHandle sampler,
                                               const char *debug_name);

    /**
     * @brief Holds the three default 1x1 textures and their bindless indices.
     *
     * These provide neutral values for missing material texture slots:
     * the shader can always sample without special-casing.
     */
    struct DefaultTextures {
        /** @brief 1x1 (1,1,1,1) — neutral base color / metallic-roughness / occlusion. */
        TextureResult white;

        /** @brief 1x1 (0.5,0.5,1,1) — tangent-space Z-up, no perturbation. */
        TextureResult flat_normal;

        /** @brief 1x1 (0,0,0,1) — no emission. */
        TextureResult black;
    };

    /**
     * @brief Creates three default 1x1 textures and registers them to the bindless array.
     *
     * Must be called within a Context::begin_immediate() / end_immediate() scope.
     * All textures are R8G8B8A8_UNORM (pixel values are at extremes where
     * SRGB and linear interpretations agree).
     *
     * @param resource_manager   RHI resource manager for image creation and upload.
     * @param descriptor_manager Descriptor manager for bindless registration.
     * @param sampler            Sampler to pair with the textures.
     * @return DefaultTextures holding the three image handles and bindless indices.
     */
    [[nodiscard]] DefaultTextures create_default_textures(rhi::ResourceManager &resource_manager,
                                                          rhi::DescriptorManager &descriptor_manager,
                                                          rhi::SamplerHandle sampler);
} // namespace himalaya::framework
