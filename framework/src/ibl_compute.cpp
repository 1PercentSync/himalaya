/**
 * @file ibl_compute.cpp
 * @brief IBL compute shader passes — equirect conversion, irradiance, prefilter, BRDF LUT, mip stripping.
 */

#include <himalaya/framework/ibl.h>
#include <himalaya/framework/cubemap_filter.h>
#include <himalaya/rhi/context.h>
#include <himalaya/rhi/resources.h>
#include <himalaya/rhi/pipeline.h>
#include <himalaya/rhi/commands.h>
#include <himalaya/rhi/shader.h>

#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cassert>
#include <cmath>
#include <vector>

namespace himalaya::framework {
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
            .compare_enable = false,
            .compare_op = rhi::CompareOp::Never,
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
        cmd.push_compute_descriptor_set(pipeline.layout, 0, writes);

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
            .compare_enable = false,
            .compare_op = rhi::CompareOp::Never,
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
        cmd.push_compute_descriptor_set(pipeline.layout, 0, writes);

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

        // Delegate dispatch to the shared framework utility
        prefilter_cubemap(ctx, *rm_, sc, cubemap_, prefiltered_cubemap_, mip_count, deferred);
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
        cmd.push_compute_descriptor_set(pipeline.layout, 0, writes);

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
} // namespace himalaya::framework
