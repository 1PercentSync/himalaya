/**
 * @file ibl.cpp
 * @brief IBL module — orchestration, HDR loading, fallback, bindless registration.
 */

#include <himalaya/framework/ibl.h>
#include <himalaya/framework/cache.h>
#include <himalaya/framework/ktx2.h>
#include <himalaya/rhi/context.h>
#include <himalaya/rhi/resources.h>
#include <himalaya/rhi/descriptors.h>

#include <glm/gtc/packing.hpp>
#include <spdlog/spdlog.h>
#include <stb_image.h>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <numbers>
#include <vector>

namespace {
    // Holds readback result for a single IBL product.
    // Staging buffer is self-managed (not pushed to Context's cleanup list)
    // because data must survive end_immediate() for CPU-side cache writing.
    struct ReadbackProduct {
        VkBuffer buffer = VK_NULL_HANDLE;
        VmaAllocation allocation = VK_NULL_HANDLE;
        void *mapped_data = nullptr;
        himalaya::rhi::Format format{};
        uint32_t base_width = 0;
        uint32_t base_height = 0;
        uint32_t face_count = 0;
        uint32_t level_count = 0;
        uint64_t total_size = 0;

        struct LevelRegion {
            uint64_t offset;
            uint64_t size;
        };

        std::vector<LevelRegion> levels;
    };

    // Records GPU->CPU readback commands for an image into the active immediate scope.
    // Creates a GpuToCpu staging buffer that the caller must destroy after reading.
    ReadbackProduct readback_image(const himalaya::rhi::Context &ctx,
                                   const himalaya::rhi::ResourceManager &rm,
                                   const himalaya::rhi::ImageHandle image) {
        const auto &img = rm.get_image(image);
        const auto &desc = img.desc;
        const uint32_t bytes_per_block = himalaya::rhi::format_bytes_per_block(desc.format);
        const auto [block_w, block_h] = himalaya::rhi::format_block_extent(desc.format);
        const uint32_t face_count = desc.array_layers;

        ReadbackProduct result;
        result.format = desc.format;
        result.base_width = desc.width;
        result.base_height = desc.height;
        result.face_count = face_count;
        result.level_count = desc.mip_levels;
        result.levels.resize(desc.mip_levels);

        // Calculate per-level sizes and total.
        // For block-compressed formats, size = blocks_x * blocks_y * bytes_per_block.
        uint64_t offset = 0;
        for (uint32_t mip = 0; mip < desc.mip_levels; ++mip) {
            const uint32_t w = std::max(desc.width >> mip, 1u);
            const uint32_t h = std::max(desc.height >> mip, 1u);
            const uint32_t blocks_x = (w + block_w - 1) / block_w;
            const uint32_t blocks_y = (h + block_h - 1) / block_h;
            const uint64_t level_size = static_cast<uint64_t>(blocks_x) * blocks_y * face_count * bytes_per_block;
            result.levels[mip] = {offset, level_size};
            offset += level_size;
        }
        result.total_size = offset;

        // Create staging buffer (GpuToCpu, mapped for CPU read after GPU completion)
        VkBufferCreateInfo buffer_ci{};
        buffer_ci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        buffer_ci.size = result.total_size;
        buffer_ci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        buffer_ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        VmaAllocationCreateInfo alloc_ci{};
        alloc_ci.usage = VMA_MEMORY_USAGE_AUTO;
        alloc_ci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT
                         | VMA_ALLOCATION_CREATE_MAPPED_BIT;

        VmaAllocationInfo alloc_info{};
        VK_CHECK(vmaCreateBuffer(ctx.allocator,
            &buffer_ci, &alloc_ci,
            &result.buffer, &result.allocation,
            &alloc_info));
        result.mapped_data = alloc_info.pMappedData;

        // Record commands
        // ReSharper disable once CppLocalVariableMayBeConst
        VkCommandBuffer cmd = ctx.immediate_command_buffer;

        // Transition SHADER_READ_ONLY -> TRANSFER_SRC
        VkImageMemoryBarrier2 to_src{};
        to_src.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        to_src.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        to_src.srcAccessMask = VK_ACCESS_2_NONE;
        to_src.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        to_src.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
        to_src.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        to_src.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        to_src.image = img.image;
        to_src.subresourceRange = {
            VK_IMAGE_ASPECT_COLOR_BIT,
            0, desc.mip_levels,
            0, face_count
        };

        VkDependencyInfo dep{};
        dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dep.imageMemoryBarrierCount = 1;
        dep.pImageMemoryBarriers = &to_src;
        vkCmdPipelineBarrier2(cmd, &dep);

        // Record copy regions (one per mip level, all faces at once)
        std::vector<VkBufferImageCopy2> regions(desc.mip_levels);
        for (uint32_t mip = 0; mip < desc.mip_levels; ++mip) {
            auto &r = regions[mip];
            r.sType = VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2;
            r.pNext = nullptr;
            r.bufferOffset = result.levels[mip].offset;
            r.bufferRowLength = 0;
            r.bufferImageHeight = 0;
            r.imageSubresource = {
                VK_IMAGE_ASPECT_COLOR_BIT,
                mip, 0, face_count
            };
            r.imageOffset = {0, 0, 0};
            r.imageExtent = {
                std::max(desc.width >> mip, 1u),
                std::max(desc.height >> mip, 1u),
                1
            };
        }

        VkCopyImageToBufferInfo2 copy_info{};
        copy_info.sType = VK_STRUCTURE_TYPE_COPY_IMAGE_TO_BUFFER_INFO_2;
        copy_info.srcImage = img.image;
        copy_info.srcImageLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        copy_info.dstBuffer = result.buffer;
        copy_info.regionCount = static_cast<uint32_t>(regions.size());
        copy_info.pRegions = regions.data();
        vkCmdCopyImageToBuffer2(cmd, &copy_info);

        // Transition TRANSFER_SRC -> SHADER_READ_ONLY (restore for rendering)
        VkImageMemoryBarrier2 to_read{};
        to_read.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        to_read.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        to_read.srcAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
        to_read.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        to_read.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        to_read.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        to_read.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        to_read.image = img.image;
        to_read.subresourceRange = {
            VK_IMAGE_ASPECT_COLOR_BIT,
            0, desc.mip_levels,
            0, face_count
        };

        dep.pImageMemoryBarriers = &to_read;
        vkCmdPipelineBarrier2(cmd, &dep);

        return result;
    }

    void destroy_readback(VmaAllocator allocator, const ReadbackProduct &product) {
        if (product.buffer != VK_NULL_HANDLE) {
            vmaDestroyBuffer(allocator, product.buffer, product.allocation);
        }
    }

    // Writes a single readback product to a KTX2 cache file.
    void write_readback_cache(const ReadbackProduct &product,
                              // ReSharper disable once CppDFAConstantParameter
                              const std::string_view category,
                              const std::string_view cache_key) {
        const auto path = himalaya::framework::cache_path(category, cache_key, ".ktx2");

        std::vector<himalaya::framework::Ktx2WriteLevel> write_levels(product.level_count);
        const auto *base = static_cast<const uint8_t *>(product.mapped_data);
        for (uint32_t i = 0; i < product.level_count; ++i) {
            write_levels[i] = {
                base + product.levels[i].offset,
                product.levels[i].size,
            };
        }

        if (!himalaya::framework::write_ktx2(path, product.format,
                                             product.base_width, product.base_height,
                                             product.face_count, write_levels)) {
            spdlog::warn("IBL: failed to write cache: {}", path.string());
        } else {
            spdlog::info("IBL: cached {} ({:.1f} MB)",
                         path.filename().string(),
                         static_cast<double>(product.total_size) / (1024.0 * 1024.0));
        }
    }

    // Creates a GPU image from KTX2 data and records upload commands.
    // Must be called within an active immediate scope.
    void upload_ktx2_image(himalaya::rhi::ResourceManager &rm,
                           const himalaya::framework::Ktx2Data &ktx2,
                           himalaya::rhi::ImageHandle &out_handle,
                           const char *debug_name) {
        const himalaya::rhi::ImageDesc desc{
            .width = ktx2.base_width,
            .height = ktx2.base_height,
            .depth = 1,
            .mip_levels = ktx2.level_count,
            .array_layers = ktx2.face_count,
            .sample_count = 1,
            .format = ktx2.format,
            .usage = himalaya::rhi::ImageUsage::Sampled
                     | himalaya::rhi::ImageUsage::TransferDst,
        };
        out_handle = rm.create_image(desc, debug_name);

        std::vector<himalaya::rhi::ResourceManager::MipUploadRegion> regions(ktx2.level_count);
        for (uint32_t i = 0; i < ktx2.level_count; ++i) {
            regions[i] = {
                .buffer_offset = ktx2.levels[i].offset,
                .width = std::max(1u, ktx2.base_width >> i),
                .height = std::max(1u, ktx2.base_height >> i),
            };
        }

        rm.upload_image_all_levels(out_handle, ktx2.blob.data(), ktx2.blob.size(),
                                   regions, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT);
    }

    // Pairs a readback product with its cache key for deferred writing.
    struct PendingCache {
        ReadbackProduct product;
        std::string cache_key;
    };
} // anonymous namespace

namespace himalaya::framework {
    bool IBL::init(rhi::Context &ctx,
                   rhi::ResourceManager &rm,
                   rhi::DescriptorManager &dm,
                   rhi::ShaderCompiler &sc,
                   const std::string &hdr_path) {
        rm_ = &rm;
        dm_ = &dm;

        // Read equirect dimensions from HDR header (no pixel decode)
        {
            int w = 0, h = 0;
            if (!hdr_path.empty() && stbi_info(hdr_path.c_str(), &w, &h, nullptr)) {
                equirect_width_ = static_cast<uint32_t>(w);
                equirect_height_ = static_cast<uint32_t>(h);
            }
        }

        // --- Phase 1: Check caches (CPU only, before any GPU work) ---
        const auto hdr_hash = content_hash(std::filesystem::path(hdr_path));

        // Try reading cubemap KTX2 files (all 3 must succeed for cache hit)
        std::optional<Ktx2Data> skybox_ktx2, irr_ktx2, pref_ktx2;
        bool cubemaps_cached = false;
        if (!hdr_hash.empty()) {
            skybox_ktx2 = read_ktx2(cache_path("ibl", hdr_hash + "_skybox", ".ktx2"));
            irr_ktx2 = read_ktx2(cache_path("ibl", hdr_hash + "_irradiance", ".ktx2"));
            pref_ktx2 = read_ktx2(cache_path("ibl", hdr_hash + "_prefiltered", ".ktx2"));
            cubemaps_cached = skybox_ktx2 && irr_ktx2 && pref_ktx2;
            if (!cubemaps_cached) {
                skybox_ktx2.reset();
                irr_ktx2.reset();
                pref_ktx2.reset();
            }
        }

        // Try reading BRDF LUT KTX2 (fixed key, HDR-independent)
        auto brdf_ktx2 = read_ktx2(cache_path("ibl", "brdf_lut", ".ktx2"));
        const bool brdf_cached = brdf_ktx2.has_value();

        // Try reading alias table binary cache (independent of cubemap cache)
        std::vector<uint8_t> alias_cache_data;
        bool alias_cached = false;
        if (!hdr_hash.empty()) {
            const auto alias_path = cache_path("ibl", hdr_hash + "_alias_table", ".bin");
            if (std::ifstream alias_file(alias_path, std::ios::binary | std::ios::ate); alias_file) {
                // Minimum valid size: header (8 bytes) + at least 1 entry (8 bytes)
                if (const auto file_size = static_cast<uint64_t>(alias_file.tellg()); file_size >= 16) {
                    alias_cache_data.resize(file_size);
                    alias_file.seekg(0);
                    alias_file.read(reinterpret_cast<char *>(alias_cache_data.data()),
                                    static_cast<std::streamsize>(file_size));
                    alias_cached = alias_file.good();
                }
            }
        }

        // --- Phase 2: GPU operations (upload cached or compute missing) ---
        DeferredCleanup deferred;
        rhi::ImageHandle equirect;
        bool hdr_loaded = cubemaps_cached; // cached cubemaps imply valid HDR
        float *hdr_rgb_data = nullptr; // Raw HDR pixels (owned, freed after alias table)
        std::vector<uint8_t> alias_build_data; // Freshly built alias table (for cache write)

        ctx.begin_immediate();

        // Cubemaps: cache load or compute
        if (cubemaps_cached) {
            upload_ktx2_image(rm, *skybox_ktx2, cubemap_, "IBL Cubemap (cached)");
            upload_ktx2_image(rm, *irr_ktx2, irradiance_cubemap_, "IBL Irradiance (cached)");
            upload_ktx2_image(rm, *pref_ktx2, prefiltered_cubemap_, "IBL Prefiltered (cached)");
            prefiltered_mip_count_ = pref_ktx2->level_count;
            // Free KTX2 blobs now (data already copied to staging buffers)
            skybox_ktx2.reset();
            irr_ktx2.reset();
            pref_ktx2.reset();
            spdlog::info("IBL: 3 cubemaps loaded from cache");
        } else {
            auto result = load_equirect(hdr_path);
            equirect = result.image;
            hdr_loaded = equirect.valid();
            hdr_rgb_data = result.rgb_data;
            // ReSharper disable once CppDFAConstantConditions
            if (hdr_loaded) {
                // ReSharper disable once CppDFAUnreachableCode
                convert_equirect_to_cubemap(ctx, sc, equirect, result.width, deferred);
                compute_irradiance(ctx, sc, deferred);
                compute_prefiltered(ctx, sc, deferred);
                // Prefiltering done — mip chain no longer needed for sampling.
                // Replace with mip-0-only copy to free ~25% of cubemap memory.
                strip_skybox_mips(ctx, deferred);
                // GPU BC6H compression: replace uncompressed cubemaps with BC6H.
                compress_cubemaps_bc6h(ctx, sc,
                                       {
                                           {&cubemap_, "IBL Skybox BC6H"},
                                           {&prefiltered_cubemap_, "IBL Prefiltered BC6H"}
                                       },
                                       deferred);
            } else {
                create_fallback_cubemaps(ctx);
            }
        }

        // Env alias table: cache load, build from existing raw data, or stbi_loadf on demand
        if (alias_cached) {
            alias_table_width_ = equirect_width_ / 2;
            alias_table_height_ = equirect_height_ / 2;
            upload_env_alias_table(alias_cache_data.data(), alias_cache_data.size());
            spdlog::info("IBL: env alias table loaded from cache");
            // ReSharper disable once CppDFAConstantConditions
        } else if (hdr_rgb_data && hdr_loaded) {
            // Cubemaps not cached — raw HDR data already available from load_equirect
            // ReSharper disable once CppDFAUnreachableCode
            alias_build_data = build_env_alias_table(hdr_rgb_data,
                                                     static_cast<int>(equirect_width_),
                                                     static_cast<int>(equirect_height_));
            upload_env_alias_table(alias_build_data.data(), alias_build_data.size());
        } else if (hdr_loaded && equirect_width_ > 0) {
            // Cubemaps cached but alias table not — load HDR pixels just for alias table
            int aw = 0, ah = 0, ac = 0;
            if (float *alias_rgb =
                    stbi_loadf(hdr_path.c_str(), &aw, &ah, &ac, 3)) {
                alias_build_data = build_env_alias_table(alias_rgb, aw, ah);
                upload_env_alias_table(alias_build_data.data(), alias_build_data.size());
                stbi_image_free(alias_rgb);
            }
        }

        // BRDF LUT: cache load or compute
        if (brdf_cached) {
            upload_ktx2_image(rm, *brdf_ktx2, brdf_lut_, "IBL BRDF LUT (cached)");
            brdf_ktx2.reset();
            spdlog::info("IBL: BRDF LUT loaded from cache");
        } else {
            compute_brdf_lut(ctx, sc, deferred);
        }

        // --- Readback + cache write preparation (only computed products) ---
        std::vector<PendingCache> pending_writes;
        // ReSharper disable once CppDFAConstantConditions
        // ReSharper disable once CppDFAUnreachableCode
        if (!cubemaps_cached && hdr_loaded && !hdr_hash.empty()) {
            // ReSharper disable once CppDFAUnreachableCode
            pending_writes.push_back({
                readback_image(ctx, rm, cubemap_),
                hdr_hash + "_skybox"
            });
            pending_writes.push_back({
                readback_image(ctx, rm, irradiance_cubemap_),
                hdr_hash + "_irradiance"
            });
            pending_writes.push_back({
                readback_image(ctx, rm, prefiltered_cubemap_),
                hdr_hash + "_prefiltered"
            });
        }
        if (!brdf_cached) {
            pending_writes.push_back({readback_image(ctx, rm, brdf_lut_), "brdf_lut"});
        }

        ctx.end_immediate();

        // --- Phase 3: Write cache files and cleanup ---
        if (!pending_writes.empty()) {
            uint64_t total_readback = 0;
            for (const auto &pw: pending_writes) {
                total_readback += pw.product.total_size;
            }
            spdlog::info("IBL: readback {} products ({:.1f} MB total)",
                         pending_writes.size(),
                         static_cast<double>(total_readback) / (1024.0 * 1024.0));

            for (const auto &pw: pending_writes) {
                write_readback_cache(pw.product, "ibl", pw.cache_key);
                destroy_readback(ctx.allocator, pw.product);
            }
        }

        // Write alias table binary cache (if freshly built)
        if (!alias_build_data.empty() && !hdr_hash.empty()) {
            const auto alias_path = cache_path("ibl", hdr_hash + "_alias_table", ".bin");
            if (std::ofstream ofs(alias_path, std::ios::binary | std::ios::trunc); ofs) {
                ofs.write(reinterpret_cast<const char *>(alias_build_data.data()),
                          static_cast<std::streamsize>(alias_build_data.size()));
                spdlog::info("IBL: env alias table cached ({:.1f} MB)",
                             static_cast<double>(alias_build_data.size()) / (1024.0 * 1024.0));
            }
            // ReSharper disable once CppDFAUnusedValue
            alias_build_data = {}; // free memory
        }

        for (auto &fn: deferred) {
            fn();
        }
        // Free raw HDR pixels (kept alive for alias table construction)
        if (hdr_rgb_data) {
            stbi_image_free(hdr_rgb_data);
        }
        // ReSharper disable once CppDFAConstantConditions
        if (equirect.valid()) {
            // ReSharper disable once CppDFAUnreachableCode
            rm_->destroy_image(equirect);
        }

        register_bindless_resources();
        return hdr_loaded;
    }

    // -----------------------------------------------------------------------
    // load_equirect — Load .hdr file, convert RGB f32 → RGBA f16, upload GPU
    // -----------------------------------------------------------------------

    IBL::EquirectResult IBL::load_equirect(const std::string &hdr_path) const {
        // Load HDR file as RGB float32
        int w, h, channels;
        float *rgb_data = stbi_loadf(hdr_path.c_str(), &w, &h, &channels, 3);
        if (!rgb_data) {
            spdlog::error("Failed to load HDR environment map '{}': {}",
                          hdr_path, stbi_failure_reason());
            return {.image = {}, .width = 0, .height = 0, .rgb_data = nullptr};
        }
        spdlog::info("Loaded HDR environment map '{}' ({}x{})", hdr_path, w, h);

        // Convert RGB float32 → RGBA float16
        const auto pixel_count = static_cast<uint32_t>(w) * static_cast<uint32_t>(h);
        std::vector<uint16_t> rgba16(pixel_count * 4);

        // Clamp to float16 max to prevent Inf/NaN (HDR sun can exceed 65504)
        for (uint32_t i = 0; i < pixel_count; ++i) {
            constexpr float kHalfMax = 65504.0f;
            rgba16[i * 4 + 0] = glm::packHalf1x16(std::min(rgb_data[i * 3 + 0], kHalfMax));
            rgba16[i * 4 + 1] = glm::packHalf1x16(std::min(rgb_data[i * 3 + 1], kHalfMax));
            rgba16[i * 4 + 2] = glm::packHalf1x16(std::min(rgb_data[i * 3 + 2], kHalfMax));
            rgba16[i * 4 + 3] = glm::packHalf1x16(1.0f);
        }

        // NOTE: rgb_data is NOT freed here — caller owns it for alias table construction.

        // Create GPU image and upload (caller must have an active immediate scope)
        const rhi::ImageDesc desc{
            .width = static_cast<uint32_t>(w),
            .height = static_cast<uint32_t>(h),
            .depth = 1,
            .mip_levels = 1,
            .array_layers = 1,
            .sample_count = 1,
            .format = rhi::Format::R16G16B16A16Sfloat,
            .usage = rhi::ImageUsage::Sampled | rhi::ImageUsage::TransferDst,
        };

        const auto image = rm_->create_image(desc, "IBL Equirect");
        rm_->upload_image(image, rgba16.data(), rgba16.size() * sizeof(uint16_t),
                          VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);

        return {
            .image = image, .width = static_cast<uint32_t>(w),
            .height = static_cast<uint32_t>(h), .rgb_data = rgb_data
        };
    }

    // -----------------------------------------------------------------------
    // build_env_alias_table — Vose's algorithm for env importance sampling
    // -----------------------------------------------------------------------

    std::vector<uint8_t> IBL::build_env_alias_table(const float *rgb_data, const int w, const int h) {
        // Half-resolution dimensions (source width/height divided by 2)
        alias_table_width_ = static_cast<uint32_t>(w) / 2;
        alias_table_height_ = static_cast<uint32_t>(h) / 2;
        const uint32_t entry_count = alias_table_width_ * alias_table_height_;

        // --- Downsample luminance × sin(theta) weights ---
        std::vector<float> weights(entry_count);
        const auto src_h = static_cast<float>(h);
        double weight_sum = 0.0;

        for (uint32_t y = 0; y < alias_table_height_; ++y) {
            // Map alias table row to 2×2 source block top-left
            const int sy = std::clamp(static_cast<int>(y) * 2, 0, h - 2);

            // sin(theta) at block center for solid angle correction (equirectangular projection)
            const float theta = std::numbers::pi_v<float> * (static_cast<float>(sy) + 1.0f) / src_h;
            const float sin_theta = std::sin(theta);

            for (uint32_t x = 0; x < alias_table_width_; ++x) {
                const int sx = std::clamp(static_cast<int>(x) * 2, 0, w - 2);

                // 2×2 box filter: average luminance of 4 source pixels
                float lum_sum = 0.0f;
                for (int dy = 0; dy < 2; ++dy) {
                    for (int dx = 0; dx < 2; ++dx) {
                        const int idx = ((sy + dy) * w + (sx + dx)) * 3;
                        const float r = rgb_data[idx + 0];
                        const float g = rgb_data[idx + 1];
                        const float b = rgb_data[idx + 2];
                        const float lum = 0.2126f * r + 0.7152f * g + 0.0722f * b;
                        lum_sum += lum;
                    }
                }
                const float lum = lum_sum * 0.25f;
                const float weight = lum * sin_theta;

                weights[y * alias_table_width_ + x] = weight;
                weight_sum += static_cast<double>(weight);
            }
        }

        total_luminance_ = static_cast<float>(weight_sum);

        // --- Vose's alias table algorithm (O(N)) ---
        struct AliasEntry {
            float prob;
            uint32_t alias;
        };

        std::vector<AliasEntry> table(entry_count);

        // Normalize weights to average = 1.0
        const double avg = weight_sum / static_cast<double>(entry_count);
        std::vector<float> normalized(entry_count);
        for (uint32_t i = 0; i < entry_count; ++i) {
            normalized[i] = (avg > 0.0) ? static_cast<float>(static_cast<double>(weights[i]) / avg) : 1.0f;
        }

        // Partition into small (< 1) and large (>= 1) work lists
        std::vector<uint32_t> small, large;
        small.reserve(entry_count);
        large.reserve(entry_count);
        for (uint32_t i = 0; i < entry_count; ++i) {
            if (normalized[i] < 1.0f) {
                small.push_back(i);
            } else {
                large.push_back(i);
            }
        }

        // Build alias table
        while (!small.empty() && !large.empty()) {
            const uint32_t s = small.back();
            small.pop_back();
            const uint32_t l = large.back();
            large.pop_back();

            table[s].prob = normalized[s];
            table[s].alias = l;

            normalized[l] = (normalized[l] + normalized[s]) - 1.0f;

            if (normalized[l] < 1.0f) {
                small.push_back(l);
            } else {
                large.push_back(l);
            }
        }

        // Remaining entries get probability 1.0 (numerical cleanup)
        while (!large.empty()) {
            const uint32_t l = large.back();
            large.pop_back();
            table[l].prob = 1.0f;
            table[l].alias = l;
        }
        while (!small.empty()) {
            const uint32_t s = small.back();
            small.pop_back();
            table[s].prob = 1.0f;
            table[s].alias = s;
        }

        // --- Upload SSBO: header (total_luminance + entry_count) + entries ---
        constexpr uint64_t header_size = sizeof(float) + sizeof(uint32_t);
        const uint64_t entries_size = static_cast<uint64_t>(entry_count) * sizeof(AliasEntry);
        const uint64_t total_size = header_size + entries_size;

        // Build contiguous SSBO layout: [float total_luminance, uint entry_count, AliasEntry[]]
        std::vector<uint8_t> cpu_data(total_size);
        std::memcpy(cpu_data.data(), &total_luminance_, sizeof(float));
        std::memcpy(cpu_data.data() + sizeof(float), &entry_count, sizeof(uint32_t));
        std::memcpy(cpu_data.data() + header_size, table.data(), entries_size);

        spdlog::info("IBL: env alias table built {}x{} ({} entries, {:.1f} MB, total_lum={:.2f})",
                     alias_table_width_,
                     alias_table_height_, entry_count,
                     static_cast<double>(total_size) / (1024.0 * 1024.0),
                     total_luminance_);

        return cpu_data;
    }

    void IBL::upload_env_alias_table(const uint8_t *data, const uint64_t size) {
        // Parse header to populate member fields
        std::memcpy(&total_luminance_, data, sizeof(float));
        uint32_t entry_count = 0;
        std::memcpy(&entry_count, data + sizeof(float), sizeof(uint32_t));

        alias_table_buffer_ = rm_->create_buffer({
                                                     .size = size,
                                                     .usage = rhi::BufferUsage::StorageBuffer |
                                                              rhi::BufferUsage::TransferDst,
                                                     .memory = rhi::MemoryUsage::GpuOnly,
                                                 }, "Env Alias Table");

        rm_->upload_buffer(alias_table_buffer_, data, size);

        spdlog::info("IBL: env alias table uploaded ({} entries, {:.1f} MB)",
                     entry_count, static_cast<double>(size) / (1024.0 * 1024.0));
    }

    // -----------------------------------------------------------------------
    // create_fallback_cubemaps — Neutral 1×1 cubemaps when HDR loading fails
    // -----------------------------------------------------------------------

    void IBL::create_fallback_cubemaps(const rhi::Context &ctx) {
        // Skybox cubemap: 1×1 R16G16B16A16F
        const rhi::ImageDesc skybox_desc{
            .width = 1,
            .height = 1,
            .depth = 1,
            .mip_levels = 1,
            .array_layers = 6,
            .sample_count = 1,
            .format = rhi::Format::R16G16B16A16Sfloat,
            .usage = rhi::ImageUsage::Sampled | rhi::ImageUsage::TransferDst,
        };
        cubemap_ = rm_->create_image(skybox_desc, "IBL Cubemap (Fallback)");

        // Irradiance cubemap: 1×1 R11G11B10F
        const rhi::ImageDesc irradiance_desc{
            .width = 1, .height = 1, .depth = 1,
            .mip_levels = 1, .array_layers = 6,
            .sample_count = 1,
            .format = rhi::Format::B10G11R11UfloatPack32,
            .usage = rhi::ImageUsage::Sampled | rhi::ImageUsage::TransferDst,
        };
        irradiance_cubemap_ = rm_->create_image(irradiance_desc, "IBL Irradiance (Fallback)");

        // Prefiltered cubemap: 1×1 R16G16B16A16F, 1 mip
        const rhi::ImageDesc prefiltered_desc{
            .width = 1, .height = 1, .depth = 1,
            .mip_levels = 1, .array_layers = 6,
            .sample_count = 1,
            .format = rhi::Format::R16G16B16A16Sfloat,
            .usage = rhi::ImageUsage::Sampled | rhi::ImageUsage::TransferDst,
        };
        prefiltered_cubemap_ = rm_->create_image(prefiltered_desc, "IBL Prefiltered (Fallback)");
        prefiltered_mip_count_ = 1;

        // --- Fill all three cubemaps with neutral gray via vkCmdClearColorImage ---
        // ReSharper disable once CppLocalVariableMayBeConst
        VkCommandBuffer cmd = ctx.immediate_command_buffer;
        constexpr VkClearColorValue clear_color = {.float32 = {0.1f, 0.1f, 0.1f, 1.0f}};

        const VkImage images[] = {
            rm_->get_image(cubemap_).image,
            rm_->get_image(irradiance_cubemap_).image,
            rm_->get_image(prefiltered_cubemap_).image,
        };

        // Batch transition: UNDEFINED → TRANSFER_DST_OPTIMAL
        VkImageMemoryBarrier2 to_transfer[3]{};
        for (int i = 0; i < 3; ++i) {
            to_transfer[i].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
            to_transfer[i].srcStageMask = VK_PIPELINE_STAGE_2_NONE;
            to_transfer[i].srcAccessMask = VK_ACCESS_2_NONE;
            to_transfer[i].dstStageMask = VK_PIPELINE_STAGE_2_CLEAR_BIT;
            to_transfer[i].dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            to_transfer[i].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            to_transfer[i].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            to_transfer[i].image = images[i];
            to_transfer[i].subresourceRange = {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .baseMipLevel = 0, .levelCount = 1,
                .baseArrayLayer = 0, .layerCount = 6,
            };
        }

        VkDependencyInfo pre_dep{};
        pre_dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        pre_dep.imageMemoryBarrierCount = 3;
        pre_dep.pImageMemoryBarriers = to_transfer;
        vkCmdPipelineBarrier2(cmd, &pre_dep);

        // Clear all cubemap faces with neutral gray
        constexpr VkImageSubresourceRange clear_range = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel = 0, .levelCount = 1,
            .baseArrayLayer = 0, .layerCount = 6,
        };
        for (const auto img: images) {
            vkCmdClearColorImage(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                 &clear_color, 1, &clear_range);
        }

        // Batch transition: TRANSFER_DST_OPTIMAL → SHADER_READ_ONLY_OPTIMAL
        VkImageMemoryBarrier2 to_read[3]{};
        for (int i = 0; i < 3; ++i) {
            to_read[i].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
            to_read[i].srcStageMask = VK_PIPELINE_STAGE_2_CLEAR_BIT;
            to_read[i].srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            to_read[i].dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
            to_read[i].dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
            to_read[i].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            to_read[i].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            to_read[i].image = images[i];
            to_read[i].subresourceRange = {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .baseMipLevel = 0, .levelCount = 1,
                .baseArrayLayer = 0, .layerCount = 6,
            };
        }

        VkDependencyInfo post_dep{};
        post_dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        post_dep.imageMemoryBarrierCount = 3;
        post_dep.pImageMemoryBarriers = to_read;
        vkCmdPipelineBarrier2(cmd, &post_dep);

        spdlog::warn("IBL: using fallback neutral cubemaps (no HDR environment loaded)");
    }

    // -----------------------------------------------------------------------
    // register_bindless_resources — Sampler creation + Set 1 registration
    // -----------------------------------------------------------------------

    void IBL::register_bindless_resources() {
        // Shared sampler: linear filtering with mip interpolation for prefiltered cubemap.
        // Single-mip products (irradiance, skybox cubemap) are unaffected by mip settings.
        constexpr rhi::SamplerDesc sampler_desc{
            .mag_filter = rhi::Filter::Linear,
            .min_filter = rhi::Filter::Linear,
            .mip_mode = rhi::SamplerMipMode::Linear,
            .wrap_u = rhi::SamplerWrapMode::ClampToEdge,
            .wrap_v = rhi::SamplerWrapMode::ClampToEdge,
            .max_anisotropy = 0.0f,
            .max_lod = VK_LOD_CLAMP_NONE,
            .compare_enable = false,
            .compare_op = rhi::CompareOp::Never,
        };
        sampler_ = rm_->create_sampler(sampler_desc, "IBL Sampler");

        // Cubemaps → Set 1 binding 1
        skybox_cubemap_idx_ = dm_->register_cubemap(cubemap_, sampler_);
        irradiance_cubemap_idx_ = dm_->register_cubemap(irradiance_cubemap_, sampler_);
        prefiltered_cubemap_idx_ = dm_->register_cubemap(prefiltered_cubemap_, sampler_);

        // BRDF LUT (2D texture) → Set 1 binding 0
        brdf_lut_idx_ = dm_->register_texture(brdf_lut_, sampler_);

        spdlog::info("IBL: registered bindless (skybox={}, irradiance={}, prefiltered={}, brdf_lut={})",
                     skybox_cubemap_idx_.index, irradiance_cubemap_idx_.index,
                     prefiltered_cubemap_idx_.index, brdf_lut_idx_.index);
    }

    // -----------------------------------------------------------------------
    // destroy — Unregister bindless entries, then destroy images and sampler
    // -----------------------------------------------------------------------

    void IBL::destroy() const {
        if (!rm_) return;

        // Unregister bindless entries first (slots returned to free lists)
        if (skybox_cubemap_idx_.valid()) dm_->unregister_cubemap(skybox_cubemap_idx_);
        if (irradiance_cubemap_idx_.valid()) dm_->unregister_cubemap(irradiance_cubemap_idx_);
        if (prefiltered_cubemap_idx_.valid()) dm_->unregister_cubemap(prefiltered_cubemap_idx_);
        if (brdf_lut_idx_.valid()) dm_->unregister_texture(brdf_lut_idx_);

        // Destroy GPU images
        if (cubemap_.valid()) rm_->destroy_image(cubemap_);
        if (irradiance_cubemap_.valid()) rm_->destroy_image(irradiance_cubemap_);
        if (prefiltered_cubemap_.valid()) rm_->destroy_image(prefiltered_cubemap_);
        if (brdf_lut_.valid()) rm_->destroy_image(brdf_lut_);

        // Destroy alias table buffer
        if (alias_table_buffer_.valid()) rm_->destroy_buffer(alias_table_buffer_);

        // Destroy shared sampler
        if (sampler_.valid()) rm_->destroy_sampler(sampler_);
    }

    rhi::BindlessIndex IBL::irradiance_cubemap_index() const {
        return irradiance_cubemap_idx_;
    }

    rhi::BindlessIndex IBL::prefiltered_cubemap_index() const {
        return prefiltered_cubemap_idx_;
    }

    rhi::BindlessIndex IBL::brdf_lut_index() const {
        return brdf_lut_idx_;
    }

    rhi::BindlessIndex IBL::skybox_cubemap_index() const {
        return skybox_cubemap_idx_;
    }

    uint32_t IBL::prefiltered_mip_count() const {
        return prefiltered_mip_count_;
    }

    uint32_t IBL::equirect_width() const {
        return equirect_width_;
    }

    uint32_t IBL::equirect_height() const {
        return equirect_height_;
    }

    rhi::BufferHandle IBL::alias_table_buffer() const {
        return alias_table_buffer_;
    }

    float IBL::total_luminance() const {
        return total_luminance_;
    }

    uint32_t IBL::alias_table_width() const {
        return alias_table_width_;
    }

    uint32_t IBL::alias_table_height() const {
        return alias_table_height_;
    }
} // namespace himalaya::framework
