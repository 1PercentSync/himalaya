#include <himalaya/framework/texture.h>
#include <himalaya/framework/cache.h>
#include <himalaya/framework/ktx2.h>
#include <himalaya/rhi/descriptors.h>
#include <himalaya/rhi/resources.h>

#include <himalaya/bc7enc/bc7enc.h>
#include <himalaya/bc7enc/rgbcx.h>
#include <spdlog/spdlog.h>
#include <stb_image.h>
#include <stb_image_resize2.h>

#include <algorithm>
#include <cassert>
#include <cmath>
#include <filesystem>

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

    namespace {
        bool g_bc_initialized = false;

        void ensure_bc_init() {
            if (g_bc_initialized) return;
            bc7enc_compress_block_init();
            rgbcx::init();
            g_bc_initialized = true;
        }
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
        std::vector<CpuMipLevel> generate_cpu_mip_chain(
            const uint8_t *src,
            const uint32_t w,
            const uint32_t h) {
            const uint32_t aw = align4(w);
            const uint32_t ah = align4(h);
            const uint32_t level_count = static_cast<uint32_t>(
                                             std::floor(std::log2(std::max(aw, ah)))) + 1;

            std::vector<CpuMipLevel> levels(level_count);

            // Level 0: resize to 4-aligned if source isn't already
            levels[0].width = aw;
            levels[0].height = ah;
            levels[0].data.resize(static_cast<size_t>(aw) * ah * 4);
            if (aw != w || ah != h) {
                stbir_resize_uint8_linear(
                    src,
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
                stbir_resize_uint8_linear(
                    levels[i - 1].data.data(),
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

        /// Compresses one RGBA8 mip level to BC7.
        std::vector<uint8_t> compress_bc7(const uint8_t *rgba,
                                          const uint32_t w,
                                          const uint32_t h,
                                          const bool perceptual) {
            bc7enc_compress_block_params params{};
            bc7enc_compress_block_params_init(&params);
            if (perceptual) {
                bc7enc_compress_block_params_init_perceptual_weights(&params);
            }

            const uint32_t bx_count = block_count(w);
            const uint32_t by_count = block_count(h);
            std::vector<uint8_t> out(static_cast<size_t>(bx_count) * by_count * 16);

            uint8_t block_pixels[64]; // 4x4 RGBA
            for (uint32_t by = 0; by < by_count; ++by) {
                for (uint32_t bx = 0; bx < bx_count; ++bx) {
                    extract_block(rgba, w, h, bx, by, block_pixels);
                    bc7enc_compress_block(
                        out.data() + (static_cast<size_t>(by) * bx_count + bx) * 16,
                        block_pixels,
                        &params);
                }
            }
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
                    rgbcx::encode_bc5(
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

    // ---- create_texture (refactored with BC compression + KTX2 cache) ----

    TextureResult create_texture(rhi::ResourceManager &resource_manager,
                                 rhi::DescriptorManager &descriptor_manager,
                                 const ImageData &data,
                                 const TextureRole role,
                                 const rhi::SamplerHandle sampler,
                                 const char *debug_name) {
        assert(data.valid() && "ImageData must be valid");
        ensure_bc_init();

        const rhi::Format bc_format = bc_format_for_role(role);

        // Compute cache key: content hash + format suffix
        const auto hash = content_hash(data.pixels.get(), data.size_bytes());
        const auto ktx2_path = cache_path("textures", hash + format_suffix(role), ".ktx2");

        // ---- Cache hit: read KTX2 and upload directly ----
        if (std::filesystem::exists(ktx2_path)) {
            if (auto ktx2 = read_ktx2(ktx2_path); ktx2 && ktx2->format == bc_format) {
                const rhi::ImageDesc desc{
                    .width = ktx2->base_width,
                    .height = ktx2->base_height,
                    .depth = 1,
                    .mip_levels = ktx2->level_count,
                    .array_layers = 1,
                    .sample_count = 1,
                    .format = bc_format,
                    .usage = rhi::ImageUsage::Sampled | rhi::ImageUsage::TransferDst,
                };

                const auto image = resource_manager.create_image(desc, debug_name);

                // Build upload regions from KTX2 level index
                std::vector<rhi::ResourceManager::MipUploadRegion> regions(ktx2->level_count);
                for (uint32_t i = 0; i < ktx2->level_count; ++i) {
                    regions[i] = {
                        .buffer_offset = ktx2->levels[i].offset,
                        .width = std::max(1u, ktx2->base_width >> i),
                        .height = std::max(1u, ktx2->base_height >> i),
                    };
                }

                resource_manager.upload_image_all_levels(
                    image, ktx2->blob.data(), ktx2->blob.size(),
                    regions, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT);

                const auto bindless_index = descriptor_manager.register_texture(image, sampler);
                spdlog::debug("Texture cache hit: {}", debug_name);
                return {image, bindless_index};
            }
            // Cache file invalid/wrong format — fall through to compress
        }

        // ---- Cache miss: CPU mip gen → BC compress → upload → write cache ----

        // 1. Generate RGBA8 mip chain (base resized to 4-aligned if needed)
        auto mip_chain = generate_cpu_mip_chain(data.pixels.get(), data.width, data.height);
        const uint32_t base_w = mip_chain[0].width;
        const uint32_t base_h = mip_chain[0].height;
        const auto level_count = static_cast<uint32_t>(mip_chain.size());

        // 2. BC compress each mip level (serial — texture-level parallelism deferred)
        std::vector<std::vector<uint8_t>> compressed(level_count);
        for (uint32_t i = 0; i < level_count; ++i) {
            const auto &mip = mip_chain[i];
            if (role == TextureRole::Normal) {
                compressed[i] = compress_bc5(mip.data.data(), mip.width, mip.height);
            } else {
                compressed[i] = compress_bc7(mip.data.data(), mip.width, mip.height,
                                             role == TextureRole::Color);
            }
        }

        // Free RGBA8 mip chain — no longer needed after compression
        mip_chain.clear();

        // 3. Build contiguous upload buffer + upload regions
        uint64_t total_size = 0;
        for (const auto &c: compressed) {
            total_size += c.size();
        }

        std::vector<uint8_t> upload_buf(total_size);
        std::vector<rhi::ResourceManager::MipUploadRegion> regions(level_count);
        uint64_t offset = 0;
        for (uint32_t i = 0; i < level_count; ++i) {
            regions[i] = {
                .buffer_offset = offset,
                .width = std::max(1u, base_w >> i),
                .height = std::max(1u, base_h >> i),
            };
            std::memcpy(upload_buf.data() + offset, compressed[i].data(), compressed[i].size());
            offset += compressed[i].size();
        }

        // 4. Create GPU image and upload
        const rhi::ImageDesc desc{
            .width = base_w,
            .height = base_h,
            .depth = 1,
            .mip_levels = level_count,
            .array_layers = 1,
            .sample_count = 1,
            .format = bc_format,
            .usage = rhi::ImageUsage::Sampled | rhi::ImageUsage::TransferDst,
        };

        const auto image = resource_manager.create_image(desc, debug_name);
        resource_manager.upload_image_all_levels(
            image,
            upload_buf.data(),
            total_size,
            regions,
            VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT);

        const auto bindless_index = descriptor_manager.register_texture(image, sampler);

        // 5. Write KTX2 cache (non-blocking to rendering, best-effort)
        std::vector<Ktx2WriteLevel> write_levels(level_count);
        for (uint32_t i = 0; i < level_count; ++i) {
            write_levels[i] = {compressed[i].data(), compressed[i].size()};
        }
        if (!write_ktx2(ktx2_path, bc_format, base_w, base_h, 1, write_levels)) {
            spdlog::warn("Failed to write texture cache: {}", ktx2_path.string());
        }

        spdlog::debug("Texture compressed and cached: {} ({}x{}, {} mips)",
                      debug_name, base_w, base_h, level_count);
        return {image, bindless_index};
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
