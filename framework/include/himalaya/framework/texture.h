#pragma once

/**
 * @file texture.h
 * @brief Texture loading, GPU upload, mip generation, and bindless registration.
 */

#include <cstdint>
#include <memory>
#include <string>

namespace himalaya::framework {
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
} // namespace himalaya::framework
