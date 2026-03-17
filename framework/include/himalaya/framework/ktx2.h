#pragma once

/**
 * @file ktx2.h
 * @brief Minimal KTX2 reader/writer for texture and IBL caching.
 *
 * Supports a fixed set of formats (BC5, BC7, R16G16B16A16_SFLOAT,
 * B10G11R11_UFLOAT_PACK32, R16G16_UNORM) for 2D and cubemap textures
 * with mip chains. No supercompression or Basis Universal support.
 */

#include <himalaya/rhi/types.h>

#include <filesystem>
#include <optional>
#include <span>
#include <vector>

namespace himalaya::framework {
    /** @brief Data read back from a KTX2 file. */
    struct Ktx2Data {
        rhi::Format format;
        uint32_t base_width;
        uint32_t base_height;
        uint32_t face_count; ///< 1 (2D) or 6 (cubemap).
        uint32_t level_count;

        /** @brief Per-level byte region within blob. */
        struct Level {
            uint64_t offset; ///< Byte offset into blob.
            uint64_t size; ///< Byte size of this level (all faces).
        };

        std::vector<Level> levels; ///< levels[0] = base (largest), levels[N-1] = smallest.
        std::vector<uint8_t> blob; ///< Contiguous mip data (KTX2 metadata stripped).
    };

    /** @brief Per-level data descriptor for write_ktx2(). */
    struct Ktx2WriteLevel {
        const void *data; ///< This level's data (cubemap: all 6 faces concatenated).
        uint64_t size; ///< Byte size.
    };

    /**
     * @brief Writes a KTX2 file.
     *
     * levels[0] = base level (largest), levels[N-1] = smallest mip.
     * For cubemaps, each level's data must contain all 6 faces contiguous.
     *
     * @return true on success, false on write failure or unsupported format.
     */
    bool write_ktx2(const std::filesystem::path &path,
                    rhi::Format format,
                    uint32_t base_width,
                    uint32_t base_height,
                    uint32_t face_count,
                    std::span<const Ktx2WriteLevel> levels);

    /**
     * @brief Reads a KTX2 file.
     *
     * @return Ktx2Data on success, nullopt on unsupported format or corrupt file.
     *         The returned blob contains only mip data; Level::offset indexes into it.
     */
    std::optional<Ktx2Data> read_ktx2(const std::filesystem::path &path);
} // namespace himalaya::framework
