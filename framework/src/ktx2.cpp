#include <himalaya/framework/ktx2.h>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <numeric>

#include <spdlog/spdlog.h>

namespace himalaya::framework {

    // ---- KTX2 binary layout ----

    static constexpr uint8_t kKtx2Identifier[12] = {
        0xAB, 0x4B, 0x54, 0x58, 0x20, 0x32, 0x30, 0xBB, 0x0D, 0x0A, 0x1A, 0x0A
    };

    /// Maps 1:1 to the first 80 bytes of a KTX2 file (little-endian, x86/x64).
#pragma pack(push, 1)
    struct Ktx2FileHeader {
        uint8_t  identifier[12];
        // Header fields (9 × uint32)
        uint32_t vk_format;
        uint32_t type_size;
        uint32_t pixel_width;
        uint32_t pixel_height;
        uint32_t pixel_depth;
        uint32_t layer_count;
        uint32_t face_count;
        uint32_t level_count;
        uint32_t supercompression_scheme;
        // Index section (4 × uint32 + 2 × uint64)
        uint32_t dfd_byte_offset;
        uint32_t dfd_byte_length;
        uint32_t kvd_byte_offset;
        uint32_t kvd_byte_length;
        uint64_t sgd_byte_offset;
        uint64_t sgd_byte_length;
    };

    /// One entry in the per-level index (3 × uint64 = 24 bytes).
    struct Ktx2LevelIndexEntry {
        uint64_t byte_offset;
        uint64_t byte_length;
        uint64_t uncompressed_byte_length;
    };
#pragma pack(pop)

    static_assert(sizeof(Ktx2FileHeader) == 80);
    static_assert(sizeof(Ktx2LevelIndexEntry) == 24);

    // ---- KHR Data Format constants ----

    static constexpr uint8_t kDfModelRgbsda    = 1;
    static constexpr uint8_t kDfModelBc5       = 132;
    static constexpr uint8_t kDfModelBc7       = 134;
    static constexpr uint8_t kDfTransferLinear  = 1;
    static constexpr uint8_t kDfTransferSrgb    = 2;
    static constexpr uint8_t kDfPrimariesBt709  = 1;
    static constexpr uint16_t kDfVersion        = 2; // KHR Data Format 1.3

    // Sample datatype flags (upper bits of channelType byte)
    static constexpr uint8_t kDfSampleFloat    = 0x80;
    static constexpr uint8_t kDfSampleSigned   = 0x40;

    // Channel IDs (RGBSDA model; values also match BC5 model channels)
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

    /// DFD basic block header parameters.
    struct DfdBlockDesc {
        uint8_t color_model;
        uint8_t transfer;
        uint8_t texel_dim_x;   ///< 0-indexed (value 3 → 4 texels).
        uint8_t texel_dim_y;   ///< 0-indexed.
        uint8_t bytes_plane0;
    };

    // DFD section sizes
    static constexpr size_t kDfdPrefixSize      = 4;   // dfdTotalSize field
    static constexpr size_t kDfdBlockHeaderSize  = 24;  // 6 × uint32
    static constexpr size_t kDfdSampleSize       = 16;  // 4 × uint32 per sample

    /// Builds the complete DFD (including dfdTotalSize prefix) as a byte vector.
    static std::vector<uint8_t> build_dfd(
        const DfdBlockDesc &block,
        const std::vector<DfdSample> &samples) {

        const auto sample_count = static_cast<uint32_t>(samples.size());
        const uint32_t block_size = kDfdBlockHeaderSize + kDfdSampleSize * sample_count;
        const uint32_t total_size = kDfdPrefixSize + block_size;

        std::vector<uint8_t> dfd(total_size, 0);
        auto write32 = [&](const size_t off, const uint32_t v) {
            std::memcpy(dfd.data() + off, &v, 4);
        };
        auto write16 = [&](const size_t off, const uint16_t v) {
            std::memcpy(dfd.data() + off, &v, 2);
        };

        // dfdTotalSize prefix
        write32(0, total_size);

        // Block header (6 words starting after prefix)
        const size_t hdr = kDfdPrefixSize;
        write32(hdr,      0); // vendorId(0) | descriptorType(0) — Khronos basic
        write32(hdr + 4,  static_cast<uint32_t>(kDfVersion) | (block_size << 16));
        write32(hdr + 8,  static_cast<uint32_t>(block.color_model)
                         | (static_cast<uint32_t>(kDfPrimariesBt709) << 8)
                         | (static_cast<uint32_t>(block.transfer) << 16));
        write32(hdr + 12, static_cast<uint32_t>(block.texel_dim_x)
                         | (static_cast<uint32_t>(block.texel_dim_y) << 8));
        write32(hdr + 16, static_cast<uint32_t>(block.bytes_plane0));
        // Word 5 (bytesPlane[4-7]) = 0, already zeroed

        // Sample descriptors
        for (size_t i = 0; i < samples.size(); ++i) {
            const size_t base = kDfdPrefixSize + kDfdBlockHeaderSize + i * kDfdSampleSize;
            const auto &s = samples[i];
            write16(base,     s.bit_offset);
            dfd[base + 2]   = s.bit_length;
            dfd[base + 3]   = s.channel_type;
            // samplePosition[4] = 0, already zeroed
            write32(base + 8,  s.lower);
            write32(base + 12, s.upper);
        }

        return dfd;
    }

    /// Returns the DFD for a supported format. Empty vector = unsupported.
    static std::vector<uint8_t> build_dfd_for_format(const rhi::Format format) {
        switch (format) {
            case rhi::Format::Bc7UnormBlock:
                return build_dfd({kDfModelBc7, kDfTransferLinear, 3, 3, 16},
                    {{0, 127, 0, 0, UINT32_MAX}});

            case rhi::Format::Bc7SrgbBlock:
                return build_dfd({kDfModelBc7, kDfTransferSrgb, 3, 3, 16},
                    {{0, 127, 0, 0, UINT32_MAX}});

            case rhi::Format::Bc5UnormBlock:
                return build_dfd({kDfModelBc5, kDfTransferLinear, 3, 3, 16},
                    {{0,  63, kChannelR, 0, UINT32_MAX},
                     {64, 63, kChannelG, 0, UINT32_MAX}});

            case rhi::Format::R16G16B16A16Sfloat:
                return build_dfd({kDfModelRgbsda, kDfTransferLinear, 0, 0, 8},
                    {{ 0, 15, static_cast<uint8_t>(kChannelR | kDfSampleFloat | kDfSampleSigned), 0xBF800000, 0x3F800000},
                     {16, 15, static_cast<uint8_t>(kChannelG | kDfSampleFloat | kDfSampleSigned), 0xBF800000, 0x3F800000},
                     {32, 15, static_cast<uint8_t>(kChannelB | kDfSampleFloat | kDfSampleSigned), 0xBF800000, 0x3F800000},
                     {48, 15, static_cast<uint8_t>(kChannelA | kDfSampleFloat | kDfSampleSigned), 0xBF800000, 0x3F800000}});

            case rhi::Format::B10G11R11UfloatPack32:
                return build_dfd({kDfModelRgbsda, kDfTransferLinear, 0, 0, 4},
                    {{ 0, 10, static_cast<uint8_t>(kChannelR | kDfSampleFloat), 0, 0x3F800000},
                     {11, 10, static_cast<uint8_t>(kChannelG | kDfSampleFloat), 0, 0x3F800000},
                     {22,  9, static_cast<uint8_t>(kChannelB | kDfSampleFloat), 0, 0x3F800000}});

            case rhi::Format::R16G16Unorm:
                return build_dfd({kDfModelRgbsda, kDfTransferLinear, 0, 0, 4},
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
        const uint32_t block_bytes = rhi::format_bytes_per_block(format);
        const uint64_t mip_alignment = std::lcm(static_cast<uint64_t>(block_bytes), uint64_t{4});

        // 1. Fill header
        Ktx2FileHeader header{};
        std::memcpy(header.identifier, kKtx2Identifier, 12);
        header.vk_format               = static_cast<uint32_t>(rhi::to_vk_format(format));
        header.type_size               = ktx2_type_size(format);
        header.pixel_width             = base_width;
        header.pixel_height            = base_height;
        header.face_count              = face_count;
        header.level_count             = level_count;
        header.dfd_byte_offset         = sizeof(Ktx2FileHeader)
                                       + level_count * sizeof(Ktx2LevelIndexEntry);
        header.dfd_byte_length         = static_cast<uint32_t>(dfd.size());
        // pixel_depth, layer_count, supercompression_scheme,
        // kvd_*, sgd_* all zero-initialized

        // 2. Compute level index (mip data stored smallest-first per KTX2 spec)
        uint64_t data_cursor = align_up(
            header.dfd_byte_offset + header.dfd_byte_length, mip_alignment);

        std::vector<Ktx2LevelIndexEntry> level_index(level_count);
        for (uint32_t i = level_count; i-- > 0; ) {
            data_cursor = align_up(data_cursor, mip_alignment);
            level_index[i] = {
                .byte_offset = data_cursor,
                .byte_length = levels[i].size,
                .uncompressed_byte_length = levels[i].size, // no supercompression
            };
            data_cursor += levels[i].size;
        }

        // 3. Assemble file in memory
        std::vector<uint8_t> file_buf(data_cursor, 0);

        std::memcpy(file_buf.data(),
                    &header, sizeof(header));
        std::memcpy(file_buf.data() + sizeof(header),
                    level_index.data(), level_count * sizeof(Ktx2LevelIndexEntry));
        std::memcpy(file_buf.data() + header.dfd_byte_offset,
                    dfd.data(), dfd.size());

        for (uint32_t i = 0; i < level_count; ++i) {
            std::memcpy(file_buf.data() + level_index[i].byte_offset,
                        static_cast<const uint8_t *>(levels[i].data),
                        levels[i].size);
        }

        // 4. Write to disk (write-to-temp + rename for crash safety)
        auto tmp_path = path;
        tmp_path += ".tmp";

        std::ofstream ofs(tmp_path, std::ios::binary | std::ios::trunc);
        if (!ofs) {
            spdlog::error("ktx2: cannot open file for writing: {}", tmp_path.string());
            return false;
        }
        ofs.write(reinterpret_cast<const char *>(file_buf.data()),
                  static_cast<std::streamsize>(file_buf.size()));
        ofs.close();
        if (!ofs.good()) return false;

        std::error_code ec;
        std::filesystem::rename(tmp_path, path, ec);
        if (ec) {
            spdlog::error("ktx2: rename failed: {}", ec.message());
            std::filesystem::remove(tmp_path, ec);
            return false;
        }
        return true;
    }

    // ---- read_ktx2 ----

    std::optional<Ktx2Data> read_ktx2(const std::filesystem::path &path) {
        std::ifstream ifs(path, std::ios::binary | std::ios::ate);
        if (!ifs) {
            return std::nullopt;
        }

        const auto file_size = static_cast<size_t>(ifs.tellg());
        if (file_size < sizeof(Ktx2FileHeader)) {
            spdlog::warn("ktx2: file too small: {}", path.string());
            return std::nullopt;
        }

        ifs.seekg(0);
        std::vector<uint8_t> file(file_size);
        ifs.read(reinterpret_cast<char *>(file.data()), static_cast<std::streamsize>(file_size));
        if (!ifs.good()) {
            spdlog::warn("ktx2: read error: {}", path.string());
            return std::nullopt;
        }

        // 1. Parse header
        Ktx2FileHeader header;
        std::memcpy(&header, file.data(), sizeof(header));

        if (std::memcmp(header.identifier, kKtx2Identifier, 12) != 0) {
            spdlog::warn("ktx2: invalid identifier: {}", path.string());
            return std::nullopt;
        }

        const auto format = rhi::from_vk_format(static_cast<VkFormat>(header.vk_format));
        if (format == rhi::Format::Undefined) {
            spdlog::warn("ktx2: unsupported vkFormat {}: {}", header.vk_format, path.string());
            return std::nullopt;
        }

        if (header.pixel_depth != 0) {
            spdlog::warn("ktx2: 3D textures not supported: {}", path.string());
            return std::nullopt;
        }
        if (header.layer_count != 0) {
            spdlog::warn("ktx2: array textures not supported: {}", path.string());
            return std::nullopt;
        }
        if (header.face_count != 1 && header.face_count != 6) {
            spdlog::warn("ktx2: invalid faceCount {}: {}", header.face_count, path.string());
            return std::nullopt;
        }
        if (header.level_count == 0) {
            spdlog::warn("ktx2: levelCount is 0: {}", path.string());
            return std::nullopt;
        }
        if (header.supercompression_scheme != 0) {
            spdlog::warn("ktx2: supercompression not supported: {}", path.string());
            return std::nullopt;
        }

        // 2. Parse level index
        const size_t level_index_end = sizeof(header)
            + static_cast<size_t>(header.level_count) * sizeof(Ktx2LevelIndexEntry);
        if (file_size < level_index_end) {
            spdlog::warn("ktx2: file too small for level index: {}", path.string());
            return std::nullopt;
        }

        std::vector<Ktx2LevelIndexEntry> level_index(header.level_count);
        std::memcpy(level_index.data(),
                    file.data() + sizeof(header),
                    header.level_count * sizeof(Ktx2LevelIndexEntry));

        // Validate bounds and compute total mip size
        uint64_t total_mip_size = 0;
        for (uint32_t i = 0; i < header.level_count; ++i) {
            if (level_index[i].byte_offset + level_index[i].byte_length > file_size) {
                spdlog::warn("ktx2: level {} data out of bounds: {}", i, path.string());
                return std::nullopt;
            }
            total_mip_size += level_index[i].byte_length;
        }

        // 3. Extract mip data into contiguous buffer (strips KTX2 metadata)
        Ktx2Data result;
        result.format      = format;
        result.base_width  = header.pixel_width;
        result.base_height = header.pixel_height;
        result.face_count  = header.face_count;
        result.level_count = header.level_count;
        result.levels.resize(header.level_count);
        result.blob.resize(total_mip_size);

        uint64_t cursor = 0;
        for (uint32_t i = 0; i < header.level_count; ++i) {
            std::memcpy(result.blob.data() + cursor,
                        file.data() + level_index[i].byte_offset,
                        level_index[i].byte_length);
            result.levels[i] = {cursor, level_index[i].byte_length};
            cursor += level_index[i].byte_length;
        }

        return result;
    }

} // namespace himalaya::framework
