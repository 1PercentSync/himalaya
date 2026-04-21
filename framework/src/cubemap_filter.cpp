/**
 * @file cubemap_filter.cpp
 * @brief GPU cubemap prefiltering via GGX importance sampling compute shader.
 */

#include <himalaya/framework/cubemap_filter.h>
#include <himalaya/rhi/context.h>
#include <himalaya/rhi/resources.h>
#include <himalaya/rhi/pipeline.h>
#include <himalaya/rhi/commands.h>
#include <himalaya/rhi/shader.h>

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cassert>
#include <cmath>
#include <vector>

namespace himalaya::framework {
    void prefilter_cubemap(rhi::Context &ctx,
                           rhi::ResourceManager &rm,
                           rhi::ShaderCompiler &sc,
                           const rhi::ImageHandle src_cubemap,
                           const rhi::ImageHandle dst_cubemap,
                           const uint32_t mip_count,
                           std::vector<std::function<void()>> &deferred) {
        const auto &dst_img = rm.get_image(dst_cubemap);
        const uint32_t face_size = dst_img.desc.width;

        // --- Temporary sampler for cubemap sampling (trilinear, mip access for filtered IS) ---
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
        const auto temp_sampler = rm.create_sampler(sampler_desc, "Prefilter Sampler");
        deferred.emplace_back([temp_sampler, &rm] { rm.destroy_sampler(temp_sampler); });

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

        vkDestroyShaderModule(ctx.device, shader_module, nullptr);
        deferred.emplace_back([device = ctx.device, pipeline, push_layout] {
            pipeline.destroy(device);
            vkDestroyDescriptorSetLayout(device, push_layout, nullptr);
        });

        // --- Create per-mip 2D array views for storage writes ---
        std::vector<VkImageView> mip_views(mip_count);
        for (uint32_t mip = 0; mip < mip_count; ++mip) {
            VkImageViewCreateInfo view_ci{};
            view_ci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            view_ci.image = dst_img.image;
            view_ci.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
            view_ci.format = rhi::to_vk_format(dst_img.desc.format);
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

        // Transition ALL mip levels UNDEFINED -> GENERAL for storage writes
        VkImageMemoryBarrier2 to_general{};
        to_general.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        to_general.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
        to_general.srcAccessMask = VK_ACCESS_2_NONE;
        to_general.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        to_general.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        to_general.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        to_general.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        to_general.image = dst_img.image;
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

        const auto &src_img = rm.get_image(src_cubemap);
        const VkSampler vk_sampler = rm.get_sampler(temp_sampler).sampler;

        VkDescriptorImageInfo input_info{};
        input_info.sampler = vk_sampler;
        input_info.imageView = src_img.view;
        input_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        for (uint32_t mip = 0; mip < mip_count; ++mip) {
            constexpr uint32_t kGroupSize = 16;
            const float mip_roughness = (mip_count <= 1)
                ? 0.0f
                : static_cast<float>(mip) / static_cast<float>(mip_count - 1);
            const uint32_t mip_size = face_size >> mip;

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
            cmd.push_compute_descriptor_set(pipeline.layout, 0, writes);

            const uint32_t groups = (mip_size + kGroupSize - 1) / kGroupSize;
            cmd.dispatch(std::max(groups, 1u), std::max(groups, 1u), 6);
        }

        // Transition ALL mip levels GENERAL -> SHADER_READ_ONLY
        // dstStage = COMPUTE because the immediate consumer is compress_bc6h().
        // IBL path also calls this but submits a new command buffer afterwards,
        // so the implicit submission-order guarantee covers fragment shader reads.
        VkImageMemoryBarrier2 to_read{};
        to_read.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        to_read.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        to_read.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        to_read.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        to_read.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        to_read.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        to_read.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        to_read.image = dst_img.image;
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

        spdlog::info("Prefiltered cubemap ({}x{} per face, {} mip levels)",
                     face_size, face_size, mip_count);
    }
} // namespace himalaya::framework
