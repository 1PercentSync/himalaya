#include <himalaya/framework/ktx2.h>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <numeric>

#include <spdlog/spdlog.h>

namespace himalaya::framework {

    // ---- KTX2 file format constants ----

    static constexpr uint8_t kKtx2Identifier[12] = {
        0xAB, 0x4B, 0x54, 0x58, 0x20, 0x32, 0x30, 0xBB, 0x0D, 0x0A, 0x1A, 0x0A
    };

    static constexpr uint32_t kHeaderSize = 80;  // identifier(12) + header(36) + index(32)

    // ---- KHR Data Format constants ----

    static constexpr uint8_t kDfModelRgbsda  = 1;
    static constexpr uint8_t kDfModelBc5     = 132;
    static constexpr uint8_t kDfModelBc7     = 134;
    static constexpr uint8_t kDfTransferLinear = 1;
    static constexpr uint8_t kDfTransferSrgb   = 2;
    static constexpr uint8_t kDfPrimariesBt709 = 1;
    static constexpr uint16_t kDfVersion13     = 2;

    // Sample datatype flags (stored in upper bits of channelType byte)
    static constexpr uint8_t kDfSampleFloat  = 0x80;
    static constexpr uint8_t kDfSampleSigned = 0x40;

    // RGBSDA channel IDs
    static constexpr uint8_t kChannelR = 0;
    static constexpr uint8_t kChannelG = 1;
    static constexpr uint8_t kChannelB = 2;
    static constexpr uint8_t kChannelA = 15;

    // ---- DFD builder ----

    /// One sample descriptor within a DFD basic block.
    struct DfdSample {
        uint16_t bit_offset;
        uint8_t  bit_length;    ///< 0-indexed (actual bits - 1).
        uint8_t  channel_type;  ///< Channel ID | datatype flags.
        uint32_t lower;
        uint32_t upper;
    };

    /// Builds the complete DFD (including dfdTotalSize prefix) as a byte vector.
    static std::vector<uint8_t> build_dfd(
        const uint8_t color_model,
        const uint8_t transfer_function,
        const uint8_t texel_dim_x, const uint8_t texel_dim_y,
        const uint8_t bytes_plane0,
        const std::vector<DfdSample> &samples) {

        const uint32_t descriptor_block_size = 24 + 16 * static_cast<uint32_t>(samples.size());
        const uint32_t dfd_total_size = 4 + descriptor_block_size;

        std::vector<uint8_t> dfd(dfd_total_size, 0);
        auto write32 = [&](const size_t off, const uint32_t v) {
            std::memcpy(dfd.data() + off, &v, 4);
        };
        auto write16 = [&](const size_t off, const uint16_t v) {
            std::memcpy(dfd.data() + off, &v, 2);
        };

        // dfdTotalSize
        write32(0, dfd_total_size);

        // Descriptor block header (offset 4)
        // Word 0: vendorId(0:16)=0 | descriptorType(17:31)=0
        write32(4, 0);
        // Word 1: versionNumber(0:15) | descriptorBlockSize(16:31)
        write32(8, static_cast<uint32_t>(kDfVersion13)
                 | (descriptor_block_size << 16));
        // Word 2: colorModel(0:7) | primaries(8:15) | transfer(16:23) | flags(24:31)
        write32(12, static_cast<uint32_t>(color_model)
                  | (static_cast<uint32_t>(kDfPrimariesBt709) << 8)
                  | (static_cast<uint32_t>(transfer_function) << 16)
                  | (0u << 24)); // flags = 0 (alpha straight)
        // Word 3: texelBlockDimension[4] (0-indexed)
        write32(16, static_cast<uint32_t>(texel_dim_x)
                  | (static_cast<uint32_t>(texel_dim_y) << 8));
        // Word 4: bytesPlane[0-3]
        write32(20, static_cast<uint32_t>(bytes_plane0));
        // Word 5: bytesPlane[4-7] = 0 (already zeroed)

        // Sample descriptors (offset 28 within DFD = 4 + 24 header bytes)
        for (size_t i = 0; i < samples.size(); ++i) {
            const size_t base = 28 + i * 16;
            const auto &s = samples[i];
            // Word 0: bitOffset(0:15) | bitLength(16:23) | channelType(24:31)
            write16(base, s.bit_offset);
            dfd[base + 2] = s.bit_length;
            dfd[base + 3] = s.channel_type;
            // Word 1: samplePosition[4] = 0 (already zeroed)
            // Word 2: sampleLower
            write32(base + 8, s.lower);
            // Word 3: sampleUpper
            write32(base + 12, s.upper);
        }

        return dfd;
    }

    /// Returns the DFD for a supported format. Empty vector = unsupported.
    static std::vector<uint8_t> build_dfd_for_format(const rhi::Format format) {
        switch (format) {
            case rhi::Format::Bc7UnormBlock:
                return build_dfd(kDfModelBc7, kDfTransferLinear, 3, 3, 16,
                    {{0, 127, 0, 0, UINT32_MAX}});

            case rhi::Format::Bc7SrgbBlock:
                return build_dfd(kDfModelBc7, kDfTransferSrgb, 3, 3, 16,
                    {{0, 127, 0, 0, UINT32_MAX}});

            case rhi::Format::Bc5UnormBlock:
                return build_dfd(kDfModelBc5, kDfTransferLinear, 3, 3, 16,
                    {{0,  63, kChannelR, 0, UINT32_MAX},
                     {64, 63, kChannelG, 0, UINT32_MAX}});

            case rhi::Format::R16G16B16A16Sfloat:
                return build_dfd(kDfModelRgbsda, kDfTransferLinear, 0, 0, 8,
                    {{ 0, 15, static_cast<uint8_t>(kChannelR | kDfSampleFloat | kDfSampleSigned), 0xBF800000, 0x3F800000},
                     {16, 15, static_cast<uint8_t>(kChannelG | kDfSampleFloat | kDfSampleSigned), 0xBF800000, 0x3F800000},
                     {32, 15, static_cast<uint8_t>(kChannelB | kDfSampleFloat | kDfSampleSigned), 0xBF800000, 0x3F800000},
                     {48, 15, static_cast<uint8_t>(kChannelA | kDfSampleFloat | kDfSampleSigned), 0xBF800000, 0x3F800000}});

            case rhi::Format::B10G11R11UfloatPack32:
                return build_dfd(kDfModelRgbsda, kDfTransferLinear, 0, 0, 4,
                    {{ 0, 10, static_cast<uint8_t>(kChannelR | kDfSampleFloat), 0, 0x3F800000},
                     {11, 10, static_cast<uint8_t>(kChannelG | kDfSampleFloat), 0, 0x3F800000},
                     {22,  9, static_cast<uint8_t>(kChannelB | kDfSampleFloat), 0, 0x3F800000}});

            case rhi::Format::R16G16Unorm:
                return build_dfd(kDfModelRgbsda, kDfTransferLinear, 0, 0, 4,
                    {{ 0, 15, kChannelR, 0, 0xFFFF},
                     {16, 15, kChannelG, 0, 0xFFFF}});

            default:
                return {};
        }
    }

    /// Returns the KTX2 typeSize for a format.
    static uint32_t ktx2_type_size(const rhi::Format format) {
        if (rhi::format_is_block_compressed(format)) return 1;
        switch (format) {
            case rhi::Format::R16G16Unorm:
            case rhi::Format::R16G16B16A16Sfloat: return 2;
            case rhi::Format::B10G11R11UfloatPack32: return 4;
            default: return 1;
        }
    }

    // ---- Alignment ----

    static uint64_t align_up(const uint64_t value, const uint64_t alignment) {
        return (value + alignment - 1) & ~(alignment - 1);
    }

    // ---- write_ktx2 ----

    bool write_ktx2(const std::filesystem::path &path,
                    const rhi::Format format,
                    const uint32_t base_width,
                    const uint32_t base_height,
                    const uint32_t face_count,
                    const std::span<const Ktx2WriteLevel> levels) {
        if (levels.empty()) return false;
        if (face_count != 1 && face_count != 6) return false;

        const auto dfd = build_dfd_for_format(format);
        if (dfd.empty()) {
            spdlog::error("ktx2: unsupported format for write");
            return false;
        }

        const auto level_count = static_cast<uint32_t>(levels.size());
        const uint32_t level_index_size = level_count * 24; // 3 × uint64 per level
        const uint32_t block_bytes = rhi::format_bytes_per_block(format);
        const uint64_t mip_alignment = std::lcm(static_cast<uint64_t>(block_bytes), uint64_t{4});

        // Compute DFD offset: right after level index
        const uint32_t dfd_offset = kHeaderSize + level_index_size;
        const auto dfd_length = static_cast<uint32_t>(dfd.size());

        // Compute mip data start: after DFD, aligned
        uint64_t data_cursor = align_up(dfd_offset + dfd_length, mip_alignment);

        // KTX2 stores mip data smallest-first. Build per-level file offsets.
        struct LevelEntry {
            uint64_t byte_offset;
            uint64_t byte_length;
        };
        std::vector<LevelEntry> level_entries(level_count);

        // First pass: compute offsets for levels stored smallest-to-largest in file
        for (uint32_t i = level_count; i-- > 0; ) {
            data_cursor = align_up(data_cursor, mip_alignment);
            level_entries[i].byte_offset = data_cursor;
            level_entries[i].byte_length = levels[i].size;
            data_cursor += levels[i].size;
        }

        const auto total_file_size = data_cursor;

        // Build the file in memory
        std::vector<uint8_t> file_buf(total_file_size, 0);
        auto write8 = [&](const size_t off, const uint8_t *src, const size_t n) {
            std::memcpy(file_buf.data() + off, src, n);
        };
        auto write32 = [&](const size_t off, const uint32_t v) {
            std::memcpy(file_buf.data() + off, &v, 4);
        };
        auto write64 = [&](const size_t off, const uint64_t v) {
            std::memcpy(file_buf.data() + off, &v, 8);
        };

        // 1. Identifier
        write8(0, kKtx2Identifier, 12);

        // 2. Header fields
        write32(12, static_cast<uint32_t>(rhi::to_vk_format(format)));  // vkFormat
        write32(16, ktx2_type_size(format));                             // typeSize
        write32(20, base_width);                                         // pixelWidth
        write32(24, base_height);                                        // pixelHeight
        write32(28, 0);                                                  // pixelDepth (2D)
        write32(32, 0);                                                  // layerCount (non-array)
        write32(36, face_count);                                         // faceCount
        write32(40, level_count);                                        // levelCount
        write32(44, 0);                                                  // supercompressionScheme (none)

        // 3. Index section
        write32(48, dfd_offset);    // dfdByteOffset
        write32(52, dfd_length);    // dfdByteLength
        write32(56, 0);             // kvdByteOffset (none)
        write32(60, 0);             // kvdByteLength (none)
        write64(64, 0);             // sgdByteOffset (none)
        write64(72, 0);             // sgdByteLength (none)

        // 4. Level index (offset 80)
        for (uint32_t i = 0; i < level_count; ++i) {
            const size_t off = kHeaderSize + i * 24;
            write64(off,      level_entries[i].byte_offset);
            write64(off + 8,  level_entries[i].byte_length);
            write64(off + 16, level_entries[i].byte_length); // uncompressed = compressed (no supercompression)
        }

        // 5. DFD
        write8(dfd_offset, dfd.data(), dfd.size());

        // 6. Mip data (file stores smallest-first, but we write via offset)
        for (uint32_t i = 0; i < level_count; ++i) {
            write8(static_cast<size_t>(level_entries[i].byte_offset),
                   static_cast<const uint8_t *>(levels[i].data),
                   static_cast<size_t>(levels[i].size));
        }

        // Write to disk
        std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
        if (!ofs) {
            spdlog::error("ktx2: cannot open file for writing: {}", path.string());
            return false;
        }
        ofs.write(reinterpret_cast<const char *>(file_buf.data()),
                  static_cast<std::streamsize>(file_buf.size()));
        return ofs.good();
    }

    // ---- read_ktx2 ----

    std::optional<Ktx2Data> read_ktx2(const std::filesystem::path &path) {
        std::ifstream ifs(path, std::ios::binary | std::ios::ate);
        if (!ifs) {
            spdlog::warn("ktx2: cannot open file: {}", path.string());
            return std::nullopt;
        }

        const auto file_size = static_cast<size_t>(ifs.tellg());
        if (file_size < kHeaderSize) {
            spdlog::warn("ktx2: file too small: {}", path.string());
            return std::nullopt;
        }

        ifs.seekg(0);
        std::vector<uint8_t> blob(file_size);
        ifs.read(reinterpret_cast<char *>(blob.data()), static_cast<std::streamsize>(file_size));
        if (!ifs.good()) {
            spdlog::warn("ktx2: read error: {}", path.string());
            return std::nullopt;
        }

        auto read32 = [&](const size_t off) -> uint32_t {
            uint32_t v;
            std::memcpy(&v, blob.data() + off, 4);
            return v;
        };
        auto read64 = [&](const size_t off) -> uint64_t {
            uint64_t v;
            std::memcpy(&v, blob.data() + off, 8);
            return v;
        };

        // 1. Validate identifier
        if (std::memcmp(blob.data(), kKtx2Identifier, 12) != 0) {
            spdlog::warn("ktx2: invalid identifier: {}", path.string());
            return std::nullopt;
        }

        // 2. Parse header
        const auto vk_format = static_cast<VkFormat>(read32(12));
        const auto format = rhi::from_vk_format(vk_format);
        if (format == rhi::Format::Undefined) {
            spdlog::warn("ktx2: unsupported vkFormat {}: {}", read32(12), path.string());
            return std::nullopt;
        }

        const uint32_t pixel_width  = read32(20);
        const uint32_t pixel_height = read32(24);
        const uint32_t pixel_depth  = read32(28);
        const uint32_t layer_count  = read32(32);
        const uint32_t face_count   = read32(36);
        const uint32_t level_count  = read32(40);
        const uint32_t supercompression = read32(44);

        if (pixel_depth != 0) {
            spdlog::warn("ktx2: 3D textures not supported: {}", path.string());
            return std::nullopt;
        }
        if (layer_count != 0) {
            spdlog::warn("ktx2: array textures not supported: {}", path.string());
            return std::nullopt;
        }
        if (face_count != 1 && face_count != 6) {
            spdlog::warn("ktx2: invalid faceCount {}: {}", face_count, path.string());
            return std::nullopt;
        }
        if (level_count == 0) {
            spdlog::warn("ktx2: levelCount is 0: {}", path.string());
            return std::nullopt;
        }
        if (supercompression != 0) {
            spdlog::warn("ktx2: supercompression not supported: {}", path.string());
            return std::nullopt;
        }

        // 3. Parse level index (starts at offset 80)
        const size_t level_index_end = kHeaderSize + static_cast<size_t>(level_count) * 24;
        if (file_size < level_index_end) {
            spdlog::warn("ktx2: file too small for level index: {}", path.string());
            return std::nullopt;
        }

        Ktx2Data result;
        result.format = format;
        result.base_width = pixel_width;
        result.base_height = pixel_height;
        result.face_count = face_count;
        result.level_count = level_count;
        result.levels.resize(level_count);

        for (uint32_t i = 0; i < level_count; ++i) {
            const size_t off = kHeaderSize + static_cast<size_t>(i) * 24;
            const uint64_t byte_offset = read64(off);
            const uint64_t byte_length = read64(off + 8);

            if (byte_offset + byte_length > file_size) {
                spdlog::warn("ktx2: level {} data out of bounds: {}", i, path.string());
                return std::nullopt;
            }

            result.levels[i] = {byte_offset, byte_length};
        }

        result.blob = std::move(blob);
        return result;
    }

} // namespace himalaya::framework
