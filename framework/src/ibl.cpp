/**
 * @file ibl.cpp
 * @brief IBL precomputation implementation.
 *
 * Loads an equirectangular .hdr environment map, converts it to a cubemap on the GPU,
 * then generates irradiance, prefiltered environment, and BRDF integration LUT via
 * compute shaders. All GPU work is recorded in a single begin_immediate() /
 * end_immediate() scope. Transient resources (pipelines, views) are collected via
 * DeferredCleanup and destroyed after GPU completion.
 */

#include <himalaya/framework/ibl.h>
#include <himalaya/framework/cache.h>
#include <himalaya/framework/ktx2.h>
#include <himalaya/rhi/context.h>
#include <himalaya/rhi/resources.h>
#include <himalaya/rhi/descriptors.h>
#include <himalaya/rhi/pipeline.h>
#include <himalaya/rhi/commands.h>
#include <himalaya/rhi/shader.h>

#include <glm/gtc/packing.hpp>
#include <spdlog/spdlog.h>
#include <stb_image.h>

#include <algorithm>
#include <bit>
#include <cassert>
#include <cmath>
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

        // --- Phase 2: GPU operations (upload cached or compute missing) ---
        DeferredCleanup deferred;
        rhi::ImageHandle equirect;
        bool hdr_loaded = cubemaps_cached; // cached cubemaps imply valid HDR

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
            // Old caches may store full mip chains; strip if present.
            strip_skybox_mips(ctx, deferred);
        } else {
            auto [eq, eq_width] = load_equirect(hdr_path);
            equirect = eq;
            hdr_loaded = equirect.valid();
            if (hdr_loaded) {
                convert_equirect_to_cubemap(ctx, sc, equirect, eq_width, deferred);
                compute_irradiance(ctx, sc, deferred);
                compute_prefiltered(ctx, sc, deferred);
                // Prefiltering done — mip chain no longer needed for sampling.
                // Replace with mip-0-only copy to free ~25% of cubemap memory.
                strip_skybox_mips(ctx, deferred);
                // GPU BC6H compression: replace uncompressed cubemaps with BC6H.
                compress_cubemap_bc6h(ctx, sc, cubemap_, deferred);
                compress_cubemap_bc6h(ctx, sc, prefiltered_cubemap_, deferred);
            } else {
                create_fallback_cubemaps(ctx);
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
        if (!cubemaps_cached && hdr_loaded && !hdr_hash.empty()) {
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

        for (auto &fn: deferred) {
            fn();
        }
        if (equirect.valid()) {
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
            return {.image = {}, .width = 0};
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

        stbi_image_free(rgb_data);

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

        return {.image = image, .width = static_cast<uint32_t>(w)};
    }

    void IBL::convert_equirect_to_cubemap(rhi::Context &ctx,
                                          rhi::ShaderCompiler &sc,
                                          const rhi::ImageHandle equirect,
                                          const uint32_t equirect_width,
                                          DeferredCleanup &deferred) {
        // Derive cubemap face size from equirect width (360° → 90° per face)
        constexpr uint32_t kMaxCubemapSize = 2048;
        const uint32_t face_size = std::min(std::bit_ceil(equirect_width / 4),
                                            kMaxCubemapSize);

        // --- Create cubemap image (R16G16B16A16F, full mip chain for filtered sampling) ---
        const uint32_t mip_count = static_cast<uint32_t>(std::floor(std::log2(face_size))) + 1;
        const rhi::ImageDesc cubemap_desc{
            .width = face_size,
            .height = face_size,
            .depth = 1,
            .mip_levels = mip_count,
            .array_layers = 6,
            .sample_count = 1,
            .format = rhi::Format::R16G16B16A16Sfloat,
            .usage = rhi::ImageUsage::Sampled | rhi::ImageUsage::Storage
                     | rhi::ImageUsage::TransferSrc | rhi::ImageUsage::TransferDst,
        };
        cubemap_ = rm_->create_image(cubemap_desc, "IBL Cubemap");

        // --- Create temporary sampler for equirect sampling ---
        constexpr rhi::SamplerDesc sampler_desc{
            .mag_filter = rhi::Filter::Linear,
            .min_filter = rhi::Filter::Linear,
            .mip_mode = rhi::SamplerMipMode::Nearest,
            .wrap_u = rhi::SamplerWrapMode::ClampToEdge,
            .wrap_v = rhi::SamplerWrapMode::ClampToEdge,
            .max_anisotropy = 0.0f,
            .max_lod = 0.0f,
        };
        const auto temp_sampler = rm_->create_sampler(sampler_desc, "IBL Equirect Sampler");
        deferred.emplace_back([temp_sampler, rm = rm_] { rm->destroy_sampler(temp_sampler); });

        // --- Compile compute shader and create pipeline ---
        auto spirv = sc.compile_from_file("ibl/equirect_to_cubemap.comp",
                                          rhi::ShaderStage::Compute);
        assert(!spirv.empty() && "Failed to compile equirect_to_cubemap.comp");
        const auto shader_module = rhi::create_shader_module(ctx.device, spirv);

        // Push descriptor layout: set 0 with combined image sampler + storage image
        constexpr VkDescriptorSetLayoutBinding bindings[] = {
            {
                .binding = 0,
                .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                .descriptorCount = 1,
                .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
            },
            {
                .binding = 1,
                .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                .descriptorCount = 1,
                .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
            },
        };

        VkDescriptorSetLayoutCreateInfo layout_ci{};
        layout_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        layout_ci.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT;
        layout_ci.bindingCount = 2;
        layout_ci.pBindings = bindings;

        VkDescriptorSetLayout push_layout = VK_NULL_HANDLE;
        VK_CHECK(vkCreateDescriptorSetLayout(ctx.device, &layout_ci, nullptr, &push_layout));

        const rhi::ComputePipelineDesc pipeline_desc{
            .compute_shader = shader_module,
            .descriptor_set_layouts = {push_layout},
        };
        auto pipeline = rhi::create_compute_pipeline(ctx.device, pipeline_desc);
        deferred.emplace_back([device = ctx.device, pipeline] { pipeline.destroy(device); });

        // Shader module can be destroyed after pipeline creation (SPIR-V is copied).
        // Descriptor set layout must survive until command buffer execution completes
        // because the pipeline layout references it during vkCmdPushDescriptorSet.
        vkDestroyShaderModule(ctx.device, shader_module, nullptr);
        deferred.emplace_back([device = ctx.device, push_layout] {
            vkDestroyDescriptorSetLayout(device, push_layout, nullptr);
        });

        // --- Create 2D array view for storage writes (compute needs layer-addressable view) ---
        const auto &cubemap_img = rm_->get_image(cubemap_);
        VkImageViewCreateInfo array_view_ci{};
        array_view_ci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        array_view_ci.image = cubemap_img.image;
        array_view_ci.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
        array_view_ci.format = VK_FORMAT_R16G16B16A16_SFLOAT;
        array_view_ci.subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 6,
        };

        VkImageView cubemap_array_view = VK_NULL_HANDLE;
        VK_CHECK(vkCreateImageView(ctx.device, &array_view_ci, nullptr, &cubemap_array_view));
        deferred.emplace_back([device = ctx.device, cubemap_array_view] {
            vkDestroyImageView(device, cubemap_array_view, nullptr);
        });

        // --- Record barriers and dispatch ---
        const rhi::CommandBuffer cmd(ctx.immediate_command_buffer);

        // Transition cubemap UNDEFINED → GENERAL for storage writes.
        VkImageMemoryBarrier2 cubemap_to_general{};
        cubemap_to_general.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        cubemap_to_general.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
        cubemap_to_general.srcAccessMask = VK_ACCESS_2_NONE;
        cubemap_to_general.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        cubemap_to_general.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        cubemap_to_general.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        cubemap_to_general.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        cubemap_to_general.image = cubemap_img.image;
        cubemap_to_general.subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 6,
        };

        VkDependencyInfo pre_dep{};
        pre_dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        pre_dep.imageMemoryBarrierCount = 1;
        pre_dep.pImageMemoryBarriers = &cubemap_to_general;
        cmd.pipeline_barrier(pre_dep);

        // Bind pipeline and push descriptors
        cmd.bind_compute_pipeline(pipeline);

        const auto &equirect_img = rm_->get_image(equirect);
        const auto &temp_vk_sampler = rm_->get_sampler(temp_sampler).sampler;

        VkDescriptorImageInfo input_info{};
        input_info.sampler = temp_vk_sampler;
        input_info.imageView = equirect_img.view;
        input_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkDescriptorImageInfo output_info{};
        output_info.imageView = cubemap_array_view;
        output_info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        const VkWriteDescriptorSet writes[] = {
            {
                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .dstBinding = 0,
                .descriptorCount = 1,
                .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                .pImageInfo = &input_info,
            },
            {
                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .dstBinding = 1,
                .descriptorCount = 1,
                .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                .pImageInfo = &output_info,
            },
        };
        cmd.push_descriptor_set(pipeline.layout, 0, writes);

        // Dispatch: one workgroup per 16×16 texel block, 6 faces
        constexpr uint32_t kGroupSize = 16;
        const uint32_t groups = (face_size + kGroupSize - 1) / kGroupSize;
        cmd.dispatch(groups, groups, 6);

        // Cubemap GENERAL → SHADER_READ_ONLY for subsequent compute sampling
        VkImageMemoryBarrier2 cubemap_to_read{};
        cubemap_to_read.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        cubemap_to_read.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        cubemap_to_read.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        cubemap_to_read.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        cubemap_to_read.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        cubemap_to_read.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        cubemap_to_read.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        cubemap_to_read.image = cubemap_img.image;
        cubemap_to_read.subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 6,
        };

        VkDependencyInfo post_dep{};
        post_dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        post_dep.imageMemoryBarrierCount = 1;
        post_dep.pImageMemoryBarriers = &cubemap_to_read;
        cmd.pipeline_barrier(post_dep);

        // Generate mip chain via blit (mip 0 is now in SHADER_READ_ONLY).
        // generate_mips expects mip 0 in SHADER_READ_ONLY and leaves all mips
        // in SHADER_READ_ONLY on completion.
        rm_->generate_mips(cubemap_);

        spdlog::info("IBL: equirect ({}px wide) -> cubemap ({}x{} per face, {} mips)",
                     equirect_width, face_size, face_size, mip_count);
    }

    // -----------------------------------------------------------------------
    // compute_irradiance — Cosine-weighted hemisphere convolution
    // -----------------------------------------------------------------------

    void IBL::compute_irradiance(rhi::Context &ctx,
                                 rhi::ShaderCompiler &sc,
                                 DeferredCleanup &deferred) {
        constexpr uint32_t kFaceSize = 32;

        // --- Create irradiance cubemap (32x32, R11G11B10F) ---
        const rhi::ImageDesc irradiance_desc{
            .width = kFaceSize,
            .height = kFaceSize,
            .depth = 1,
            .mip_levels = 1,
            .array_layers = 6,
            .sample_count = 1,
            .format = rhi::Format::B10G11R11UfloatPack32,
            .usage = rhi::ImageUsage::Sampled | rhi::ImageUsage::Storage | rhi::ImageUsage::TransferSrc,
        };
        irradiance_cubemap_ = rm_->create_image(irradiance_desc, "IBL Irradiance");

        // --- Temporary sampler for cubemap sampling ---
        constexpr rhi::SamplerDesc sampler_desc{
            .mag_filter = rhi::Filter::Linear,
            .min_filter = rhi::Filter::Linear,
            .mip_mode = rhi::SamplerMipMode::Nearest,
            .wrap_u = rhi::SamplerWrapMode::ClampToEdge,
            .wrap_v = rhi::SamplerWrapMode::ClampToEdge,
            .max_anisotropy = 0.0f,
            .max_lod = 0.0f,
        };
        const auto temp_sampler = rm_->create_sampler(sampler_desc, "IBL Irradiance Sampler");
        deferred.emplace_back([temp_sampler, rm = rm_] { rm->destroy_sampler(temp_sampler); });

        // --- Compile compute shader and create pipeline ---
        auto spirv = sc.compile_from_file("ibl/irradiance.comp",
                                          rhi::ShaderStage::Compute);
        assert(!spirv.empty() && "Failed to compile irradiance.comp");
        const auto shader_module = rhi::create_shader_module(ctx.device, spirv);

        // Push descriptor layout: combined image sampler (cubemap input) + storage image (output)
        constexpr VkDescriptorSetLayoutBinding bindings[] = {
            {
                .binding = 0,
                .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                .descriptorCount = 1,
                .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
            },
            {
                .binding = 1,
                .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                .descriptorCount = 1,
                .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
            },
        };

        VkDescriptorSetLayoutCreateInfo layout_ci{};
        layout_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        layout_ci.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT;
        layout_ci.bindingCount = 2;
        layout_ci.pBindings = bindings;

        VkDescriptorSetLayout push_layout = VK_NULL_HANDLE;
        VK_CHECK(vkCreateDescriptorSetLayout(ctx.device, &layout_ci, nullptr, &push_layout));

        const rhi::ComputePipelineDesc pipeline_desc{
            .compute_shader = shader_module,
            .descriptor_set_layouts = {push_layout},
        };
        auto pipeline = rhi::create_compute_pipeline(ctx.device, pipeline_desc);
        deferred.emplace_back([device = ctx.device, pipeline] { pipeline.destroy(device); });

        vkDestroyShaderModule(ctx.device, shader_module, nullptr);
        deferred.emplace_back([device = ctx.device, push_layout] {
            vkDestroyDescriptorSetLayout(device, push_layout, nullptr);
        });

        // --- Create 2D array view for storage writes ---
        const auto &irradiance_img = rm_->get_image(irradiance_cubemap_);
        VkImageViewCreateInfo array_view_ci{};
        array_view_ci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        array_view_ci.image = irradiance_img.image;
        array_view_ci.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
        array_view_ci.format = VK_FORMAT_B10G11R11_UFLOAT_PACK32;
        array_view_ci.subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 6,
        };

        VkImageView irradiance_array_view = VK_NULL_HANDLE;
        VK_CHECK(vkCreateImageView(ctx.device, &array_view_ci, nullptr, &irradiance_array_view));
        deferred.emplace_back([device = ctx.device, irradiance_array_view] {
            vkDestroyImageView(device, irradiance_array_view, nullptr);
        });

        // --- Record barriers and dispatch ---
        const rhi::CommandBuffer cmd(ctx.immediate_command_buffer);

        // Transition irradiance UNDEFINED → GENERAL for storage writes
        VkImageMemoryBarrier2 irr_to_general{};
        irr_to_general.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        irr_to_general.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
        irr_to_general.srcAccessMask = VK_ACCESS_2_NONE;
        irr_to_general.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        irr_to_general.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        irr_to_general.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        irr_to_general.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        irr_to_general.image = irradiance_img.image;
        irr_to_general.subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 6,
        };

        VkDependencyInfo pre_dep{};
        pre_dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        pre_dep.imageMemoryBarrierCount = 1;
        pre_dep.pImageMemoryBarriers = &irr_to_general;
        cmd.pipeline_barrier(pre_dep);

        // Bind pipeline and push descriptors
        cmd.bind_compute_pipeline(pipeline);

        const auto &cubemap_img = rm_->get_image(cubemap_);
        const auto &temp_vk_sampler = rm_->get_sampler(temp_sampler).sampler;

        VkDescriptorImageInfo input_info{};
        input_info.sampler = temp_vk_sampler;
        input_info.imageView = cubemap_img.view;
        input_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkDescriptorImageInfo output_info{};
        output_info.imageView = irradiance_array_view;
        output_info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        const VkWriteDescriptorSet writes[] = {
            {
                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .dstBinding = 0,
                .descriptorCount = 1,
                .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                .pImageInfo = &input_info,
            },
            {
                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .dstBinding = 1,
                .descriptorCount = 1,
                .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                .pImageInfo = &output_info,
            },
        };
        cmd.push_descriptor_set(pipeline.layout, 0, writes);

        // Dispatch: one workgroup per 16x16 texel block, 6 faces
        constexpr uint32_t kGroupSize = 16;
        constexpr uint32_t kGroups = (kFaceSize + kGroupSize - 1) / kGroupSize;
        cmd.dispatch(kGroups, kGroups, 6);

        // Irradiance GENERAL → SHADER_READ_ONLY for subsequent compute sampling
        VkImageMemoryBarrier2 irr_to_read{};
        irr_to_read.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        irr_to_read.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        irr_to_read.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        irr_to_read.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        irr_to_read.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        irr_to_read.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        irr_to_read.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        irr_to_read.image = irradiance_img.image;
        irr_to_read.subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 6,
        };

        VkDependencyInfo post_dep{};
        post_dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        post_dep.imageMemoryBarrierCount = 1;
        post_dep.pImageMemoryBarriers = &irr_to_read;
        cmd.pipeline_barrier(post_dep);

        spdlog::info("IBL: irradiance cubemap ({}x{} per face, R11G11B10F)",
                     kFaceSize, kFaceSize);
    }

    // -----------------------------------------------------------------------
    // compute_prefiltered — GGX importance-sampled specular prefilter
    // -----------------------------------------------------------------------

    void IBL::compute_prefiltered(rhi::Context &ctx,
                                  rhi::ShaderCompiler &sc,
                                  DeferredCleanup &deferred) {
        constexpr uint32_t kFaceSize = 512;
        const uint32_t mip_count = static_cast<uint32_t>(std::floor(std::log2(kFaceSize))) + 1;
        prefiltered_mip_count_ = mip_count;

        // --- Create prefiltered cubemap (512x512, full mip chain, R16G16B16A16F) ---
        const rhi::ImageDesc prefiltered_desc{
            .width = kFaceSize,
            .height = kFaceSize,
            .depth = 1,
            .mip_levels = mip_count,
            .array_layers = 6,
            .sample_count = 1,
            .format = rhi::Format::R16G16B16A16Sfloat,
            .usage = rhi::ImageUsage::Sampled | rhi::ImageUsage::Storage | rhi::ImageUsage::TransferSrc,
        };
        prefiltered_cubemap_ = rm_->create_image(prefiltered_desc, "IBL Prefiltered");

        // --- Temporary sampler for cubemap sampling (mip access for filtered IS) ---
        constexpr rhi::SamplerDesc sampler_desc{
            .mag_filter = rhi::Filter::Linear,
            .min_filter = rhi::Filter::Linear,
            .mip_mode = rhi::SamplerMipMode::Linear,
            .wrap_u = rhi::SamplerWrapMode::ClampToEdge,
            .wrap_v = rhi::SamplerWrapMode::ClampToEdge,
            .max_anisotropy = 0.0f,
            .max_lod = VK_LOD_CLAMP_NONE,
        };
        const auto temp_sampler = rm_->create_sampler(sampler_desc, "IBL Prefilter Sampler");
        deferred.emplace_back([temp_sampler, rm = rm_] { rm->destroy_sampler(temp_sampler); });

        // --- Compile compute shader and create pipeline (with push constant for roughness) ---
        auto spirv = sc.compile_from_file("ibl/prefilter.comp",
                                          rhi::ShaderStage::Compute);
        assert(!spirv.empty() && "Failed to compile prefilter.comp");
        const auto shader_module = rhi::create_shader_module(ctx.device, spirv);

        constexpr VkDescriptorSetLayoutBinding bindings[] = {
            {
                .binding = 0,
                .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                .descriptorCount = 1,
                .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
            },
            {
                .binding = 1,
                .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                .descriptorCount = 1,
                .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
            },
        };

        VkDescriptorSetLayoutCreateInfo layout_ci{};
        layout_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        layout_ci.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT;
        layout_ci.bindingCount = 2;
        layout_ci.pBindings = bindings;

        VkDescriptorSetLayout push_layout = VK_NULL_HANDLE;
        VK_CHECK(vkCreateDescriptorSetLayout(ctx.device, &layout_ci, nullptr, &push_layout));

        const rhi::ComputePipelineDesc pipeline_desc{
            .compute_shader = shader_module,
            .descriptor_set_layouts = {push_layout},
            .push_constant_ranges = {
                {
                    .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
                    .offset = 0,
                    .size = sizeof(float),
                }
            },
        };
        auto pipeline = rhi::create_compute_pipeline(ctx.device, pipeline_desc);
        deferred.emplace_back([device = ctx.device, pipeline] { pipeline.destroy(device); });

        vkDestroyShaderModule(ctx.device, shader_module, nullptr);
        deferred.emplace_back([device = ctx.device, push_layout] {
            vkDestroyDescriptorSetLayout(device, push_layout, nullptr);
        });

        // --- Create per-mip 2D array views for storage writes ---
        const auto &prefiltered_img = rm_->get_image(prefiltered_cubemap_);

        std::vector<VkImageView> mip_views(mip_count);
        for (uint32_t mip = 0; mip < mip_count; ++mip) {
            VkImageViewCreateInfo view_ci{};
            view_ci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            view_ci.image = prefiltered_img.image;
            view_ci.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
            view_ci.format = VK_FORMAT_R16G16B16A16_SFLOAT;
            view_ci.subresourceRange = {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .baseMipLevel = mip,
                .levelCount = 1,
                .baseArrayLayer = 0,
                .layerCount = 6,
            };
            VK_CHECK(vkCreateImageView(ctx.device, &view_ci, nullptr, &mip_views[mip]));
            deferred.emplace_back([device = ctx.device, view = mip_views[mip]] {
                vkDestroyImageView(device, view, nullptr);
            });
        }

        // --- Record barriers and per-mip dispatch ---
        const rhi::CommandBuffer cmd(ctx.immediate_command_buffer);

        // Transition ALL mip levels UNDEFINED → GENERAL
        VkImageMemoryBarrier2 to_general{};
        to_general.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        to_general.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
        to_general.srcAccessMask = VK_ACCESS_2_NONE;
        to_general.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        to_general.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        to_general.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        to_general.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        to_general.image = prefiltered_img.image;
        to_general.subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel = 0,
            .levelCount = mip_count,
            .baseArrayLayer = 0,
            .layerCount = 6,
        };

        VkDependencyInfo pre_dep{};
        pre_dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        pre_dep.imageMemoryBarrierCount = 1;
        pre_dep.pImageMemoryBarriers = &to_general;
        cmd.pipeline_barrier(pre_dep);

        cmd.bind_compute_pipeline(pipeline);

        const auto &cubemap_img = rm_->get_image(cubemap_);
        const auto &temp_vk_sampler = rm_->get_sampler(temp_sampler).sampler;

        VkDescriptorImageInfo input_info{};
        input_info.sampler = temp_vk_sampler;
        input_info.imageView = cubemap_img.view;
        input_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        for (uint32_t mip = 0; mip < mip_count; ++mip) {
            constexpr uint32_t kGroupSize = 16;
            const float mip_roughness = static_cast<float>(mip) / static_cast<float>(mip_count - 1);
            const uint32_t mip_size = kFaceSize >> mip;

            // Push roughness constant
            cmd.push_constants(pipeline.layout, VK_SHADER_STAGE_COMPUTE_BIT,
                               &mip_roughness, sizeof(float));

            // Push descriptors with per-mip output view
            VkDescriptorImageInfo output_info{};
            output_info.imageView = mip_views[mip];
            output_info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

            const VkWriteDescriptorSet writes[] = {
                {
                    .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                    .dstBinding = 0,
                    .descriptorCount = 1,
                    .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                    .pImageInfo = &input_info,
                },
                {
                    .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                    .dstBinding = 1,
                    .descriptorCount = 1,
                    .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                    .pImageInfo = &output_info,
                },
            };
            cmd.push_descriptor_set(pipeline.layout, 0, writes);

            const uint32_t groups = (mip_size + kGroupSize - 1) / kGroupSize;
            cmd.dispatch(std::max(groups, 1u), std::max(groups, 1u), 6);
        }

        // Transition ALL mip levels GENERAL → SHADER_READ_ONLY
        VkImageMemoryBarrier2 to_read{};
        to_read.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        to_read.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        to_read.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        to_read.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        to_read.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        to_read.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        to_read.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        to_read.image = prefiltered_img.image;
        to_read.subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel = 0,
            .levelCount = mip_count,
            .baseArrayLayer = 0,
            .layerCount = 6,
        };

        VkDependencyInfo post_dep{};
        post_dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        post_dep.imageMemoryBarrierCount = 1;
        post_dep.pImageMemoryBarriers = &to_read;
        cmd.pipeline_barrier(post_dep);

        spdlog::info("IBL: prefiltered cubemap ({}x{} per face, {} mip levels)",
                     kFaceSize, kFaceSize, mip_count);
    }

    // -----------------------------------------------------------------------
    // strip_skybox_mips — drop mip 1..N after prefiltering no longer needs them
    // -----------------------------------------------------------------------

    void IBL::strip_skybox_mips(const rhi::Context &ctx, DeferredCleanup &deferred) {
        const auto &old_img = rm_->get_image(cubemap_);
        if (old_img.desc.mip_levels <= 1) return;

        // Capture values before create_image (which may reallocate the pool).
        const VkImage old_vk = old_img.image;
        const uint32_t w = old_img.desc.width;
        const uint32_t h = old_img.desc.height;
        const rhi::Format fmt = old_img.desc.format;
        const uint32_t old_mips = old_img.desc.mip_levels;

        // Calculate memory saved (mip 1..N across all 6 faces).
        const uint32_t bpp = rhi::format_bytes_per_block(fmt);
        uint64_t stripped_bytes = 0;
        for (uint32_t m = 1; m < old_mips; ++m) {
            const uint32_t mw = std::max(1u, w >> m);
            const uint32_t mh = std::max(1u, h >> m);
            stripped_bytes += static_cast<uint64_t>(mw) * mh * 6 * bpp;
        }

        // Create 1-mip replacement cubemap.
        const rhi::ImageDesc new_desc{
            .width = w,
            .height = h,
            .depth = 1,
            .mip_levels = 1,
            .array_layers = 6,
            .sample_count = 1,
            .format = fmt,
            .usage = rhi::ImageUsage::Sampled
                     | rhi::ImageUsage::TransferSrc
                     | rhi::ImageUsage::TransferDst,
        };
        auto new_cubemap = rm_->create_image(new_desc, "IBL Cubemap");
        const VkImage new_vk = rm_->get_image(new_cubemap).image;

        // ReSharper disable once CppLocalVariableMayBeConst
        VkCommandBuffer cmd = ctx.immediate_command_buffer;

        // Barrier: old SHADER_READ_ONLY → TRANSFER_SRC (mip 0 only),
        //          new UNDEFINED → TRANSFER_DST.
        std::array<VkImageMemoryBarrier2, 2> pre{};
        pre[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        pre[0].srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        pre[0].srcAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        pre[0].dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        pre[0].dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
        pre[0].oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        pre[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        pre[0].image = old_vk;
        pre[0].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 6};

        pre[1].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        pre[1].srcStageMask = VK_PIPELINE_STAGE_2_NONE;
        pre[1].srcAccessMask = VK_ACCESS_2_NONE;
        pre[1].dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        pre[1].dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        pre[1].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        pre[1].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        pre[1].image = new_vk;
        pre[1].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 6};

        VkDependencyInfo dep{};
        dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dep.imageMemoryBarrierCount = 2;
        dep.pImageMemoryBarriers = pre.data();
        vkCmdPipelineBarrier2(cmd, &dep);

        // Copy mip 0 (all 6 faces) from old → new.
        VkImageCopy2 region{};
        region.sType = VK_STRUCTURE_TYPE_IMAGE_COPY_2;
        region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 6};
        region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 6};
        region.extent = {w, h, 1};

        VkCopyImageInfo2 copy{};
        copy.sType = VK_STRUCTURE_TYPE_COPY_IMAGE_INFO_2;
        copy.srcImage = old_vk;
        copy.srcImageLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        copy.dstImage = new_vk;
        copy.dstImageLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        copy.regionCount = 1;
        copy.pRegions = &region;
        vkCmdCopyImage2(cmd, &copy);

        // Transition new cubemap to SHADER_READ_ONLY for rendering + readback.
        VkImageMemoryBarrier2 post{};
        post.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        post.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        post.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        post.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        post.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        post.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        post.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        post.image = new_vk;
        post.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 6};

        dep.imageMemoryBarrierCount = 1;
        dep.pImageMemoryBarriers = &post;
        vkCmdPipelineBarrier2(cmd, &dep);

        // Defer destruction of old cubemap (GPU may still reference it until
        // end_immediate). Swap handle now so readback + bindless use the new image.
        auto old_handle = cubemap_;
        deferred.push_back([this, old_handle]() { rm_->destroy_image(old_handle); });
        cubemap_ = new_cubemap;

        spdlog::info("IBL: stripped skybox mips ({} -> 1, freed {:.1f} MB)",
                     old_mips,
                     static_cast<double>(stripped_bytes) / (1024.0 * 1024.0));
    }

    // -----------------------------------------------------------------------
    // compress_cubemap_bc6h — GPU BC6H compression via compute shader
    // -----------------------------------------------------------------------

    void IBL::compress_cubemap_bc6h(rhi::Context &ctx,
                                    rhi::ShaderCompiler &sc,
                                    rhi::ImageHandle &handle,
                                    DeferredCleanup &deferred) {
        // Capture source properties before create_image (which may reallocate the pool).
        const auto &src_img = rm_->get_image(handle);
        const uint32_t face_w = src_img.desc.width;
        const uint32_t face_h = src_img.desc.height;
        const uint32_t mip_count = src_img.desc.mip_levels;
        const rhi::Format src_format = src_img.desc.format;
        const uint32_t src_bpp = rhi::format_bytes_per_block(src_format);
        const VkImage src_vk = src_img.image;

        // --- Create BC6H destination cubemap ---
        const rhi::ImageDesc bc6h_desc{
            .width = face_w,
            .height = face_h,
            .depth = 1,
            .mip_levels = mip_count,
            .array_layers = 6,
            .sample_count = 1,
            .format = rhi::Format::Bc6hUfloatBlock,
            .usage = rhi::ImageUsage::Sampled
                     | rhi::ImageUsage::TransferSrc
                     | rhi::ImageUsage::TransferDst,
        };
        auto bc6h_handle = rm_->create_image(bc6h_desc, "IBL BC6H Cubemap");

        // --- Compile BC6H compute shader and create pipeline ---
        auto spirv = sc.compile_from_file("compress/bc6h.comp",
                                          rhi::ShaderStage::Compute);
        assert(!spirv.empty() && "Failed to compile compress/bc6h.comp");
        const auto shader_module = rhi::create_shader_module(ctx.device, spirv);

        constexpr VkDescriptorSetLayoutBinding layout_bindings[] = {
            {
                .binding = 0,
                .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                .descriptorCount = 1,
                .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
            },
            {
                .binding = 1,
                .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                .descriptorCount = 1,
                .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
            },
        };

        VkDescriptorSetLayoutCreateInfo layout_ci{};
        layout_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        layout_ci.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT;
        layout_ci.bindingCount = 2;
        layout_ci.pBindings = layout_bindings;

        VkDescriptorSetLayout push_layout = VK_NULL_HANDLE;
        VK_CHECK(vkCreateDescriptorSetLayout(ctx.device, &layout_ci, nullptr, &push_layout));

        // Push constant range: vec2 texel_size_rcp + uint blocks_per_row = 12 bytes
        constexpr VkPushConstantRange pc_range{
            .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
            .offset = 0,
            .size = 12,
        };

        const rhi::ComputePipelineDesc pipeline_desc{
            .compute_shader = shader_module,
            .descriptor_set_layouts = {push_layout},
            .push_constant_ranges = {pc_range},
        };
        auto pipeline = rhi::create_compute_pipeline(ctx.device, pipeline_desc);

        vkDestroyShaderModule(ctx.device, shader_module, nullptr);
        deferred.emplace_back([device = ctx.device, pipeline, push_layout] {
            pipeline.destroy(device);
            vkDestroyDescriptorSetLayout(device, push_layout, nullptr);
        });

        // --- Create nearest sampler for source sampling ---
        constexpr rhi::SamplerDesc sampler_desc{
            .mag_filter = rhi::Filter::Nearest,
            .min_filter = rhi::Filter::Nearest,
            .mip_mode = rhi::SamplerMipMode::Nearest,
            .wrap_u = rhi::SamplerWrapMode::ClampToEdge,
            .wrap_v = rhi::SamplerWrapMode::ClampToEdge,
            .max_anisotropy = 0.0f,
            .max_lod = 0.0f,
        };
        const auto temp_sampler = rm_->create_sampler(sampler_desc, "BC6H Src Sampler");
        deferred.emplace_back([temp_sampler, rm = rm_] { rm->destroy_sampler(temp_sampler); });
        const VkSampler vk_sampler = rm_->get_sampler(temp_sampler).sampler;

        // --- Create staging buffer (sized for largest mip: base face) ---
        const uint32_t max_blocks_w = (face_w + 3) / 4;
        const uint32_t max_blocks_h = (face_h + 3) / 4;
        const VkDeviceSize buffer_size = static_cast<VkDeviceSize>(max_blocks_w) * max_blocks_h * 16;

        VkBufferCreateInfo buf_ci{};
        buf_ci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        buf_ci.size = buffer_size;
        buf_ci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        buf_ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        VmaAllocationCreateInfo alloc_ci{};
        alloc_ci.usage = VMA_MEMORY_USAGE_AUTO;

        VkBuffer staging_buf = VK_NULL_HANDLE;
        VmaAllocation staging_alloc = VK_NULL_HANDLE;
        VK_CHECK(vmaCreateBuffer(ctx.allocator, &buf_ci, &alloc_ci,
            &staging_buf, &staging_alloc, nullptr));
        deferred.emplace_back([a = ctx.allocator, staging_buf, staging_alloc] {
            vmaDestroyBuffer(a, staging_buf, staging_alloc);
        });

        // --- Record commands ---
        const rhi::CommandBuffer cmd(ctx.immediate_command_buffer);

        VkImageMemoryBarrier2 dst_to_xfer{};
        dst_to_xfer.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        dst_to_xfer.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
        dst_to_xfer.srcAccessMask = VK_ACCESS_2_NONE;
        dst_to_xfer.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        dst_to_xfer.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        dst_to_xfer.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        dst_to_xfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        dst_to_xfer.image = rm_->get_image(bc6h_handle).image;
        dst_to_xfer.subresourceRange = {
            VK_IMAGE_ASPECT_COLOR_BIT, 0, mip_count, 0, 6
        };

        VkDependencyInfo dep{};
        dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dep.imageMemoryBarrierCount = 1;
        dep.pImageMemoryBarriers = &dst_to_xfer;
        cmd.pipeline_barrier(dep);

        // Bind pipeline (shared across all dispatches)
        cmd.bind_compute_pipeline(pipeline);

        // --- Per face × per mip: create view, dispatch, barrier, copy ---
        constexpr uint32_t kGroupSize = 8;

        for (uint32_t mip = 0; mip < mip_count; ++mip) {
            const uint32_t mip_w = std::max(1u, face_w >> mip);
            const uint32_t mip_h = std::max(1u, face_h >> mip);
            const uint32_t blocks_w = (mip_w + 3) / 4;
            const uint32_t blocks_h = (mip_h + 3) / 4;
            const VkDeviceSize mip_buf_size = static_cast<VkDeviceSize>(blocks_w) * blocks_h * 16;

            for (uint32_t face = 0; face < 6; ++face) {
                // Create per-face 2D view of source cubemap at this mip
                VkImageViewCreateInfo view_ci{};
                view_ci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
                view_ci.image = src_vk;
                view_ci.viewType = VK_IMAGE_VIEW_TYPE_2D;
                view_ci.format = rhi::to_vk_format(src_format);
                view_ci.subresourceRange = {
                    VK_IMAGE_ASPECT_COLOR_BIT, mip, 1, face, 1
                };

                VkImageView face_view = VK_NULL_HANDLE;
                VK_CHECK(vkCreateImageView(ctx.device, &view_ci, nullptr, &face_view));
                deferred.emplace_back([device = ctx.device, face_view] {
                    vkDestroyImageView(device, face_view, nullptr);
                });

                // Push descriptors: binding 0 = source face, binding 1 = SSBO
                VkDescriptorImageInfo src_info{};
                src_info.sampler = vk_sampler;
                src_info.imageView = face_view;
                src_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

                VkDescriptorBufferInfo dst_info{};
                dst_info.buffer = staging_buf;
                dst_info.offset = 0;
                dst_info.range = mip_buf_size;

                const VkWriteDescriptorSet writes[] = {
                    {
                        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                        .dstBinding = 0,
                        .descriptorCount = 1,
                        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                        .pImageInfo = &src_info,
                    },
                    {
                        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                        .dstBinding = 1,
                        .descriptorCount = 1,
                        .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                        .pBufferInfo = &dst_info,
                    },
                };
                cmd.push_descriptor_set(pipeline.layout, 0, writes);

                // Push constants
                struct {
                    float texel_size_rcp[2];
                    uint32_t blocks_per_row;
                } pc{};
                pc.texel_size_rcp[0] = 1.0f / static_cast<float>(mip_w);
                pc.texel_size_rcp[1] = 1.0f / static_cast<float>(mip_h);
                pc.blocks_per_row = blocks_w;
                cmd.push_constants(pipeline.layout, VK_SHADER_STAGE_COMPUTE_BIT,
                                   &pc, sizeof(pc));

                // Dispatch: one thread per 4×4 block, local_size = 8×8
                const uint32_t groups_x = (blocks_w + kGroupSize - 1) / kGroupSize;
                const uint32_t groups_y = (blocks_h + kGroupSize - 1) / kGroupSize;
                cmd.dispatch(groups_x, groups_y, 1);

                // Barrier: compute write → transfer read (SSBO)
                VkMemoryBarrier2 buf_barrier{};
                buf_barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
                buf_barrier.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
                buf_barrier.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
                buf_barrier.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
                buf_barrier.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;

                VkDependencyInfo buf_dep{};
                buf_dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
                buf_dep.memoryBarrierCount = 1;
                buf_dep.pMemoryBarriers = &buf_barrier;
                cmd.pipeline_barrier(buf_dep);

                // Copy SSBO → BC6H cubemap face+mip
                VkBufferImageCopy2 copy_region{};
                copy_region.sType = VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2;
                copy_region.bufferOffset = 0;
                copy_region.bufferRowLength = 0;
                copy_region.bufferImageHeight = 0;
                copy_region.imageSubresource = {
                    VK_IMAGE_ASPECT_COLOR_BIT, mip, face, 1
                };
                copy_region.imageOffset = {0, 0, 0};
                copy_region.imageExtent = {mip_w, mip_h, 1};

                VkCopyBufferToImageInfo2 copy_info{};
                copy_info.sType = VK_STRUCTURE_TYPE_COPY_BUFFER_TO_IMAGE_INFO_2;
                copy_info.srcBuffer = staging_buf;
                copy_info.dstImage = rm_->get_image(bc6h_handle).image;
                copy_info.dstImageLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                copy_info.regionCount = 1;
                copy_info.pRegions = &copy_region;
                cmd.copy_buffer_to_image(copy_info);

                // Barrier: transfer write → compute write (reuse SSBO next iteration)
                VkMemoryBarrier2 reuse_barrier{};
                reuse_barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
                reuse_barrier.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
                reuse_barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
                reuse_barrier.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
                reuse_barrier.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;

                VkDependencyInfo reuse_dep{};
                reuse_dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
                reuse_dep.memoryBarrierCount = 1;
                reuse_dep.pMemoryBarriers = &reuse_barrier;
                cmd.pipeline_barrier(reuse_dep);
            }
        }

        // --- Transition BC6H cubemap TRANSFER_DST → SHADER_READ_ONLY ---
        VkImageMemoryBarrier2 dst_to_read{};
        dst_to_read.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        dst_to_read.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        dst_to_read.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        dst_to_read.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        dst_to_read.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        dst_to_read.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        dst_to_read.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        dst_to_read.image = rm_->get_image(bc6h_handle).image;
        dst_to_read.subresourceRange = {
            VK_IMAGE_ASPECT_COLOR_BIT, 0, mip_count, 0, 6
        };

        dep.pImageMemoryBarriers = &dst_to_read;
        cmd.pipeline_barrier(dep);

        // --- Swap handles: BC6H replaces uncompressed ---
        auto old_handle = handle;
        deferred.emplace_back([this, old_handle] { rm_->destroy_image(old_handle); });
        handle = bc6h_handle;

        // Calculate compression ratio for logging
        uint64_t uncompressed_bytes = 0;
        for (uint32_t m = 0; m < mip_count; ++m) {
            const uint32_t mw = std::max(1u, face_w >> m);
            const uint32_t mh = std::max(1u, face_h >> m);
            uncompressed_bytes += static_cast<uint64_t>(mw) * mh * 6 * src_bpp;
        }
        uint64_t compressed_bytes = 0;
        for (uint32_t m = 0; m < mip_count; ++m) {
            const uint32_t mw = std::max(1u, face_w >> m);
            const uint32_t mh = std::max(1u, face_h >> m);
            compressed_bytes += static_cast<uint64_t>((mw + 3) / 4) * ((mh + 3) / 4) * 6 * 16;
        }

        spdlog::info("IBL: BC6H compressed cubemap {}x{} ({} mips): {:.1f} MB -> {:.1f} MB",
                     face_w, face_h, mip_count,
                     static_cast<double>(uncompressed_bytes) / (1024.0 * 1024.0),
                     static_cast<double>(compressed_bytes) / (1024.0 * 1024.0));
    }

    // -----------------------------------------------------------------------
    // compute_brdf_lut — BRDF integration LUT for Split-Sum approximation
    // -----------------------------------------------------------------------

    void IBL::compute_brdf_lut(rhi::Context &ctx,
                               rhi::ShaderCompiler &sc,
                               DeferredCleanup &deferred) {
        constexpr uint32_t kSize = 256;

        // --- Create BRDF LUT image (256x256, R16G16_UNORM) ---
        const rhi::ImageDesc lut_desc{
            .width = kSize,
            .height = kSize,
            .depth = 1,
            .mip_levels = 1,
            .array_layers = 1,
            .sample_count = 1,
            .format = rhi::Format::R16G16Unorm,
            .usage = rhi::ImageUsage::Sampled | rhi::ImageUsage::Storage | rhi::ImageUsage::TransferSrc,
        };
        brdf_lut_ = rm_->create_image(lut_desc, "IBL BRDF LUT");

        // --- Compile compute shader and create pipeline ---
        auto spirv = sc.compile_from_file("ibl/brdf_lut.comp",
                                          rhi::ShaderStage::Compute);
        assert(!spirv.empty() && "Failed to compile brdf_lut.comp");
        const auto shader_module = rhi::create_shader_module(ctx.device, spirv);

        // Push descriptor layout: single storage image output
        constexpr VkDescriptorSetLayoutBinding binding{
            .binding = 0,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
        };

        VkDescriptorSetLayoutCreateInfo layout_ci{};
        layout_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        layout_ci.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT;
        layout_ci.bindingCount = 1;
        layout_ci.pBindings = &binding;

        VkDescriptorSetLayout push_layout = VK_NULL_HANDLE;
        VK_CHECK(vkCreateDescriptorSetLayout(ctx.device, &layout_ci, nullptr, &push_layout));

        const rhi::ComputePipelineDesc pipeline_desc{
            .compute_shader = shader_module,
            .descriptor_set_layouts = {push_layout},
        };
        auto pipeline = rhi::create_compute_pipeline(ctx.device, pipeline_desc);
        deferred.emplace_back([device = ctx.device, pipeline] { pipeline.destroy(device); });

        vkDestroyShaderModule(ctx.device, shader_module, nullptr);
        deferred.emplace_back([device = ctx.device, push_layout] {
            vkDestroyDescriptorSetLayout(device, push_layout, nullptr);
        });

        // --- Record barriers and dispatch ---
        const rhi::CommandBuffer cmd(ctx.immediate_command_buffer);
        const auto &lut_img = rm_->get_image(brdf_lut_);

        // Transition UNDEFINED → GENERAL for storage writes
        VkImageMemoryBarrier2 to_general{};
        to_general.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        to_general.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
        to_general.srcAccessMask = VK_ACCESS_2_NONE;
        to_general.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        to_general.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        to_general.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        to_general.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        to_general.image = lut_img.image;
        to_general.subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1,
        };

        VkDependencyInfo pre_dep{};
        pre_dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        pre_dep.imageMemoryBarrierCount = 1;
        pre_dep.pImageMemoryBarriers = &to_general;
        cmd.pipeline_barrier(pre_dep);

        // Bind pipeline and push descriptor
        cmd.bind_compute_pipeline(pipeline);

        VkDescriptorImageInfo output_info{};
        output_info.imageView = lut_img.view;
        output_info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        const VkWriteDescriptorSet writes[] = {
            {
                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .dstBinding = 0,
                .descriptorCount = 1,
                .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                .pImageInfo = &output_info,
            },
        };
        cmd.push_descriptor_set(pipeline.layout, 0, writes);

        constexpr uint32_t kGroupSize = 16;
        constexpr uint32_t kGroups = (kSize + kGroupSize - 1) / kGroupSize;
        cmd.dispatch(kGroups, kGroups, 1);

        // GENERAL → SHADER_READ_ONLY for runtime sampling
        VkImageMemoryBarrier2 to_read{};
        to_read.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        to_read.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        to_read.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        to_read.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        to_read.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        to_read.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        to_read.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        to_read.image = lut_img.image;
        to_read.subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1,
        };

        VkDependencyInfo post_dep{};
        post_dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        post_dep.imageMemoryBarrierCount = 1;
        post_dep.pImageMemoryBarriers = &to_read;
        cmd.pipeline_barrier(post_dep);

        spdlog::info("IBL: BRDF integration LUT ({}x{}, R16G16_UNORM)", kSize, kSize);
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
} // namespace himalaya::framework
