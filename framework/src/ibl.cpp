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
#include <vector>

namespace himalaya::framework {
    void IBL::init(rhi::Context &ctx,
                   rhi::ResourceManager &rm,
                   rhi::DescriptorManager &dm,
                   rhi::ShaderCompiler &sc,
                   const std::string &hdr_path) {
        rm_ = &rm;
        dm_ = &dm;

        DeferredCleanup deferred;

        ctx.begin_immediate();

        auto [equirect, eq_width] = load_equirect(hdr_path);
        convert_equirect_to_cubemap(ctx, sc, equirect, eq_width, deferred);
        // TODO: compute_irradiance(ctx, sc, deferred);
        // TODO: compute_prefiltered(ctx, sc, deferred);
        // TODO: compute_brdf_lut(ctx, sc, deferred);

        ctx.end_immediate();

        // Safe to destroy after GPU completion (end_immediate does vkQueueWaitIdle)
        for (auto &fn: deferred) fn();
        rm_->destroy_image(equirect);

        // TODO: register_bindless_resources();
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

        for (uint32_t i = 0; i < pixel_count; ++i) {
            rgba16[i * 4 + 0] = glm::packHalf1x16(rgb_data[i * 3 + 0]);
            rgba16[i * 4 + 1] = glm::packHalf1x16(rgb_data[i * 3 + 1]);
            rgba16[i * 4 + 2] = glm::packHalf1x16(rgb_data[i * 3 + 2]);
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
        constexpr uint32_t kMinCubemapSize = 256;
        constexpr uint32_t kMaxCubemapSize = 2048;
        const uint32_t face_size = std::clamp(std::bit_ceil(equirect_width / 4),
                                              kMinCubemapSize,
                                              kMaxCubemapSize);

        // --- Create cubemap image (R16G16B16A16F) ---
        const rhi::ImageDesc cubemap_desc{
            .width = face_size,
            .height = face_size,
            .depth = 1,
            .mip_levels = 1,
            .array_layers = 6,
            .sample_count = 1,
            .format = rhi::Format::R16G16B16A16Sfloat,
            .usage = rhi::ImageUsage::Sampled | rhi::ImageUsage::Storage,
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

        // Shader module and descriptor set layout can be destroyed immediately
        // (not referenced by command buffer during execution)
        vkDestroyShaderModule(ctx.device, shader_module, nullptr);
        vkDestroyDescriptorSetLayout(ctx.device, push_layout, nullptr);

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

        spdlog::info("IBL: equirect ({}px wide) -> cubemap ({}x{} per face)",
                     equirect_width, face_size, face_size);
    }

    void IBL::destroy() {
        if (!rm_) return;

        // TODO: Unregister bindless entries, then destroy images and sampler
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
