#include <himalaya/framework/texture.h>
#include <himalaya/framework/cache.h>
#include <himalaya/framework/ktx2.h>
#include <himalaya/rhi/descriptors.h>
#include <himalaya/rhi/resources.h>

#include <bc7e_ispc.h>
#include <himalaya/bc7enc/rgbcx.h>
#include <spdlog/spdlog.h>
#include <stb_image.h>
#include <stb_image_resize2.h>

#include <algorithm>
#include <cassert>
#include <cmath>
#include <filesystem>
#include <mutex>

namespace himalaya::framework {
    ImageData load_image(const std::string &path) {
        int w, h, channels;
        // Force 4 channels (RGBA) regardless of source format.
        auto *raw = stbi_load(path.c_str(), &w, &h, &channels, 4);
        if (!raw) {
            spdlog::error("Failed to load image '{}': {}", path, stbi_failure_reason());
            return {};
        }

        return {
            .pixels = {raw, stbi_image_free},
            .width = static_cast<uint32_t>(w),
            .height = static_cast<uint32_t>(h),
        };
    }

    ImageData load_image_from_memory(const uint8_t *buffer, const size_t byte_length) {
        int w, h, channels;
        auto *raw = stbi_load_from_memory(buffer, static_cast<int>(byte_length),
                                          &w, &h, &channels, 4);
        if (!raw) {
            spdlog::error("Failed to decode image from memory: {}", stbi_failure_reason());
            return {};
        }

        return {
            .pixels = {raw, stbi_image_free},
            .width = static_cast<uint32_t>(w),
            .height = static_cast<uint32_t>(h),
        };
    }

    // ---- BC encoder one-time initialization ----

    void ensure_bc_init() {
        static std::once_flag flag;
        std::call_once(flag, [] {
            ispc::bc7e_compress_block_init();
            rgbcx::init();
        });
    }

    // ---- CPU mip chain generation ----

    namespace {
        /// Rounds up to next multiple of 4.
        uint32_t align4(const uint32_t v) { return (v + 3) & ~3u; }

        /// One level of an RGBA8 mip chain.
        struct CpuMipLevel { // NOLINT(*-pro-type-member-init)
            std::vector<uint8_t> data;
            uint32_t width;
            uint32_t height;
        };

        /// Generates a full RGBA8 mip chain. Level 0 is the source, resized to
        /// 4-aligned dimensions if needed (for BC block alignment at base level).
        /// When srgb is true, filtering is done in linear space (gamma-correct
        /// for color textures); otherwise data is treated as linear.
        std::vector<CpuMipLevel> generate_cpu_mip_chain(
            const uint8_t *src,
            const uint32_t w,
            const uint32_t h,
            const bool srgb) {
            const uint32_t aw = align4(w);
            const uint32_t ah = align4(h);
            const uint32_t level_count = static_cast<uint32_t>(
                                             std::floor(std::log2(std::max(aw, ah)))) + 1;

            // sRGB textures must be filtered in linear space to avoid darkening
            // artifacts in lower mip levels. stbir_resize_uint8_srgb handles
            // the decode (gamma expand) → filter → encode (gamma compress) cycle.
            const auto resize = srgb
                                    ? stbir_resize_uint8_srgb
                                    : stbir_resize_uint8_linear;

            std::vector<CpuMipLevel> levels(level_count);

            // Level 0: resize to 4-aligned if source isn't already
            levels[0].width = aw;
            levels[0].height = ah;
            levels[0].data.resize(static_cast<size_t>(aw) * ah * 4);
            if (aw != w || ah != h) {
                resize(src,
                       static_cast<int>(w),
                       static_cast<int>(h),
                       0,
                       levels[0].data.data(),
                       static_cast<int>(aw),
                       static_cast<int>(ah),
                       0,
                       STBIR_RGBA);
            } else {
                std::memcpy(levels[0].data.data(), src, levels[0].data.size());
            }

            // Subsequent levels: half dimensions, min 1
            for (uint32_t i = 1; i < level_count; ++i) {
                const uint32_t pw = levels[i - 1].width;
                const uint32_t ph = levels[i - 1].height;
                const uint32_t nw = std::max(1u, pw / 2);
                const uint32_t nh = std::max(1u, ph / 2);

                levels[i].width = nw;
                levels[i].height = nh;
                levels[i].data.resize(static_cast<size_t>(nw) * nh * 4);
                resize(levels[i - 1].data.data(),
                       static_cast<int>(pw),
                       static_cast<int>(ph),
                       0,
                       levels[i].data.data(),
                       static_cast<int>(nw),
                       static_cast<int>(nh),
                       0,
                       STBIR_RGBA);
            }

            return levels;
        }
    }

    // ---- BC compression ----

    namespace {
        /// Number of BC blocks for a given pixel dimension.
        uint32_t block_count(const uint32_t pixels) {
            return (pixels + 3) / 4;
        }

        /// Extracts a 4x4 pixel block from RGBA8 data. Pads with black if the
        /// block extends past the image boundary.
        void extract_block(const uint8_t *src,
                           const uint32_t img_w,
                           const uint32_t img_h,
                           const uint32_t bx,
                           const uint32_t by,
                           uint8_t out[64]) {
            for (uint32_t y = 0; y < 4; ++y) {
                for (uint32_t x = 0; x < 4; ++x) {
                    const uint32_t px = bx * 4 + x;
                    const uint32_t py = by * 4 + y;
                    const size_t dst_off = (y * 4 + x) * 4;
                    if (px < img_w && py < img_h) {
                        const size_t src_off = (static_cast<size_t>(py) * img_w + px) * 4;
                        std::memcpy(out + dst_off, src + src_off, 4);
                    } else {
                        std::memset(out + dst_off, 0, 4);
                    }
                }
            }
        }

        /// Compresses one RGBA8 mip level to BC7 using bc7e (ISPC SIMD).
        std::vector<uint8_t> compress_bc7(const uint8_t *rgba,
                                          const uint32_t w,
                                          const uint32_t h,
                                          const bool perceptual) {
            ispc::bc7e_compress_block_params params{};
            ispc::bc7e_compress_block_params_init_slowest(&params, perceptual);

            const uint32_t bx_count = block_count(w);
            const uint32_t by_count = block_count(h);
            const uint32_t total_blocks = bx_count * by_count;

            // Pack all 4x4 blocks contiguously as uint32_t (RGBA8 per pixel).
            // bc7e processes them in parallel via ISPC gang width.
            std::vector<uint32_t> pixels(static_cast<size_t>(total_blocks) * 16);
            uint8_t tmp[64];
            for (uint32_t by = 0; by < by_count; ++by) {
                for (uint32_t bx = 0; bx < bx_count; ++bx) {
                    extract_block(rgba, w, h, bx, by, tmp);
                    const auto idx = static_cast<size_t>(by) * bx_count + bx;
                    std::memcpy(&pixels[idx * 16], tmp, 64);
                }
            }

            std::vector<uint8_t> out(static_cast<size_t>(total_blocks) * 16);
            ispc::bc7e_compress_blocks(
                total_blocks,
                reinterpret_cast<uint64_t *>(out.data()),
                pixels.data(),
                &params);
            return out;
        }

        /// Compresses one RGBA8 mip level to BC5 (RG channels only).
        std::vector<uint8_t> compress_bc5(const uint8_t *rgba, const uint32_t w, const uint32_t h) {
            const uint32_t bx_count = block_count(w);
            const uint32_t by_count = block_count(h);
            std::vector<uint8_t> out(static_cast<size_t>(bx_count) * by_count * 16);

            uint8_t block_pixels[64]; // 4x4 RGBA
            for (uint32_t by = 0; by < by_count; ++by) {
                for (uint32_t bx = 0; bx < bx_count; ++bx) {
                    extract_block(rgba, w, h, bx, by, block_pixels);
                    // BC5 = two BC4 blocks. rgbcx::encode_bc5 takes RGBA pixels,
                    // chan0=R(0), chan1=G(1), stride=4 bytes per pixel.
                    rgbcx::encode_bc5_hq(
                        out.data() + (static_cast<size_t>(by) * bx_count + bx) * 16,
                        block_pixels,
                        0,
                        1,
                        4);
                }
            }
            return out;
        }
    }

    // ---- BC format mapping ----

    namespace {
        rhi::Format bc_format_for_role(const TextureRole role) {
            switch (role) {
                case TextureRole::Color: return rhi::Format::Bc7SrgbBlock;
                case TextureRole::Linear: return rhi::Format::Bc7UnormBlock;
                case TextureRole::Normal: return rhi::Format::Bc5UnormBlock;
            }
            return rhi::Format::Bc7UnormBlock;
        }

        /// Format suffix for cache filename disambiguation.
        const char *format_suffix(const TextureRole role) {
            switch (role) {
                case TextureRole::Color: return "_bc7s";
                case TextureRole::Linear: return "_bc7u";
                case TextureRole::Normal: return "_bc5u";
            }
            return "_bc7u";
        }
    }

    // ---- Cache lookup ----

    std::optional<PreparedTexture> load_cached_texture(const std::string_view source_hash, const TextureRole role) {
        const rhi::Format bc_format = bc_format_for_role(role);
        const auto ktx2_path = cache_path(
            "textures",
            std::string(source_hash) + format_suffix(role),
            ".ktx2");

        if (!std::filesystem::exists(ktx2_path)) {
            return std::nullopt;
        }

        auto ktx2 = read_ktx2(ktx2_path);
        if (!ktx2 || ktx2->format != bc_format) {
            return std::nullopt;
        }

        PreparedTexture result;
        result.format = bc_format;
        result.base_width = ktx2->base_width;
        result.base_height = ktx2->base_height;
        result.level_count = ktx2->level_count;
        result.regions.resize(ktx2->level_count);
        for (uint32_t i = 0; i < ktx2->level_count; ++i) {
            result.regions[i] = {
                .buffer_offset = ktx2->levels[i].offset,
                .width = std::max(1u, ktx2->base_width >> i),
                .height = std::max(1u, ktx2->base_height >> i),
            };
        }
        result.data = std::move(ktx2->blob);
        return result;
    }

    // ---- CPU compression + cache write ----

    PreparedTexture compress_texture(const ImageData &data,
                                     const TextureRole role,
                                     const std::string_view source_hash) {
        assert(data.valid() && "ImageData must be valid");

        const rhi::Format bc_format = bc_format_for_role(role);

        auto mip_chain = generate_cpu_mip_chain(data.pixels.get(),
                                                data.width,
                                                data.height,
                                                role == TextureRole::Color);
        const uint32_t base_w = mip_chain[0].width;
        const uint32_t base_h = mip_chain[0].height;
        const auto level_count = static_cast<uint32_t>(mip_chain.size());

        // BC compress each mip level
        std::vector<std::vector<uint8_t> > compressed(level_count);
        for (uint32_t i = 0; i < level_count; ++i) {
            // ReSharper disable once CppUseStructuredBinding
            const auto &mip = mip_chain[i];
            if (role == TextureRole::Normal) {
                compressed[i] = compress_bc5(mip.data.data(), mip.width, mip.height);
            } else {
                compressed[i] = compress_bc7(mip.data.data(),
                                             mip.width,
                                             mip.height,
                                             role == TextureRole::Color);
            }
        }
        mip_chain.clear();

        // Build contiguous upload buffer + regions
        uint64_t total_size = 0;
        for (const auto &c: compressed) total_size += c.size();

        PreparedTexture result;
        result.format = bc_format;
        result.base_width = base_w;
        result.base_height = base_h;
        result.level_count = level_count;
        result.data.resize(total_size);
        result.regions.resize(level_count);

        uint64_t offset = 0;
        for (uint32_t i = 0; i < level_count; ++i) {
            result.regions[i] = {
                .buffer_offset = offset,
                .width = std::max(1u, base_w >> i),
                .height = std::max(1u, base_h >> i),
            };
            std::memcpy(result.data.data() + offset, compressed[i].data(), compressed[i].size());
            offset += compressed[i].size();
        }

        // Write KTX2 cache (best-effort)
        const auto ktx2_path = cache_path(
            "textures",
            std::string(source_hash) + format_suffix(role),
            ".ktx2");
        std::vector<Ktx2WriteLevel> write_levels(level_count);
        for (uint32_t i = 0; i < level_count; ++i) {
            write_levels[i] = {
                result.data.data() + result.regions[i].buffer_offset,
                (i + 1 < level_count)
                    ? result.regions[i + 1].buffer_offset - result.regions[i].buffer_offset
                    : result.data.size() - result.regions[i].buffer_offset,
            };
        }
        if (!write_ktx2(ktx2_path, bc_format, base_w, base_h, 1, write_levels)) {
            spdlog::warn("Failed to write texture cache: {}", ktx2_path.string());
        }

        return result;
    }

    // ---- prepare_texture (convenience: hash pixels, check cache, compress if miss) ----

    PreparedTexture prepare_texture(const ImageData &data, const TextureRole role) {
        assert(data.valid() && "ImageData must be valid");
        const auto hash = content_hash(data.pixels.get(), data.size_bytes());
        if (auto cached = load_cached_texture(hash, role)) {
            return std::move(*cached);
        }
        return compress_texture(data, role, hash);
    }

    // ---- finalize_texture (GPU upload + bindless + cache write) ----

    TextureResult finalize_texture(rhi::ResourceManager &resource_manager,
                                   rhi::DescriptorManager &descriptor_manager,
                                   const PreparedTexture &prepared,
                                   const rhi::SamplerHandle sampler,
                                   const char *debug_name) {
        const rhi::ImageDesc desc{
            .width = prepared.base_width,
            .height = prepared.base_height,
            .depth = 1,
            .mip_levels = prepared.level_count,
            .array_layers = 1,
            .sample_count = 1,
            .format = prepared.format,
            .usage = rhi::ImageUsage::Sampled | rhi::ImageUsage::TransferDst,
        };

        const auto image = resource_manager.create_image(desc, debug_name);

        // Convert PreparedMipRegion → MipUploadRegion (identical layout)
        std::vector<rhi::ResourceManager::MipUploadRegion> upload_regions(prepared.level_count);
        for (uint32_t i = 0; i < prepared.level_count; ++i) {
            upload_regions[i] = {
                .buffer_offset = prepared.regions[i].buffer_offset,
                .width = prepared.regions[i].width,
                .height = prepared.regions[i].height,
            };
        }
        resource_manager.upload_image_all_levels(
            image,
            prepared.data.data(),
            prepared.data.size(),
            upload_regions,
            VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT);

        const auto bindless_index = descriptor_manager.register_texture(image, sampler);
        return {image, bindless_index};
    }

    // ---- create_texture (convenience wrapper) ----

    TextureResult create_texture(rhi::ResourceManager &resource_manager,
                                 rhi::DescriptorManager &descriptor_manager,
                                 const ImageData &data,
                                 const TextureRole role,
                                 const rhi::SamplerHandle sampler,
                                 const char *debug_name) {
        ensure_bc_init();
        const auto prepared = prepare_texture(data, role);
        return finalize_texture(resource_manager, descriptor_manager, prepared, sampler, debug_name);
    }

    namespace {
        // Creates a 1x1 R8G8B8A8_UNORM texture and registers it to the bindless array.
        // Must be called within begin_immediate/end_immediate scope.
        TextureResult create_solid_texture(rhi::ResourceManager &resource_manager,
                                           rhi::DescriptorManager &descriptor_manager,
                                           const rhi::SamplerHandle sampler,
                                           const uint8_t r, const uint8_t g,
                                           // ReSharper disable once CppDFAConstantParameter
                                           const uint8_t b, const uint8_t a,
                                           const char *debug_name) {
            const rhi::ImageDesc desc{
                .width = 1,
                .height = 1,
                .depth = 1,
                .mip_levels = 1,
                .array_layers = 1,
                .sample_count = 1,
                .format = rhi::Format::R8G8B8A8Unorm,
                .usage = rhi::ImageUsage::Sampled | rhi::ImageUsage::TransferDst,
            };

            const auto image = resource_manager.create_image(desc, debug_name);
            const uint8_t pixels[4] = {r, g, b, a};
            resource_manager.upload_image(image,
                                          pixels,
                                          sizeof(pixels),
                                          VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT);

            const auto bindless_index = descriptor_manager.register_texture(image, sampler);
            return {image, bindless_index};
        }
    }

    DefaultTextures create_default_textures(rhi::ResourceManager &resource_manager,
                                            rhi::DescriptorManager &descriptor_manager,
                                            const rhi::SamplerHandle sampler) {
        DefaultTextures defaults{};
        defaults.white = create_solid_texture(resource_manager, descriptor_manager, sampler,
                                              255, 255, 255, 255, "Default White");
        // R8G8B8A8_UNORM, not BC5: 1x1 textures don't need block compression,
        // and the shader only samples .rg (RG=128 → XY≈0, Z reconstructed ≈1).
        defaults.flat_normal = create_solid_texture(resource_manager, descriptor_manager, sampler,
                                                    128, 128, 255, 255, "Default Flat Normal");
        defaults.black = create_solid_texture(resource_manager, descriptor_manager, sampler,
                                              0, 0, 0, 255, "Default Black");

        spdlog::info("Default textures created (white={}, flat_normal={}, black={})",
                     defaults.white.bindless_index.index,
                     defaults.flat_normal.bindless_index.index,
                     defaults.black.bindless_index.index);

        return defaults;
    }
} // namespace himalaya::framework
