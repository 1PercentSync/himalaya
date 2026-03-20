/**
 * @file ibl_compress.cpp
 * @brief IBL GPU BC6H compression via compute shader.
 */

#include <himalaya/framework/ibl.h>
#include <himalaya/rhi/context.h>
#include <himalaya/rhi/resources.h>
#include <himalaya/rhi/pipeline.h>
#include <himalaya/rhi/commands.h>
#include <himalaya/rhi/shader.h>

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cassert>

namespace himalaya::framework {
    // -----------------------------------------------------------------------
    // compress_cubemaps_bc6h — GPU BC6H compression via compute shader
    // -----------------------------------------------------------------------

    void IBL::compress_cubemaps_bc6h(
        rhi::Context &ctx,
        rhi::ShaderCompiler &sc,
        std::initializer_list<std::pair<rhi::ImageHandle *, const char *> > handles,
        DeferredCleanup &deferred) {
        // --- Compile BC6H compute shader and create pipeline (once) ---
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

        constexpr VkPushConstantRange pc_range{
            .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
            .offset = 0,
            .size = 16, // vec2 texel_size_rcp + uint blocks_per_row + uint blocks_per_col
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

        // --- Create nearest sampler for source sampling (shared) ---
        constexpr rhi::SamplerDesc sampler_desc{
            .mag_filter = rhi::Filter::Nearest,
            .min_filter = rhi::Filter::Nearest,
            .mip_mode = rhi::SamplerMipMode::Nearest,
            .wrap_u = rhi::SamplerWrapMode::ClampToEdge,
            .wrap_v = rhi::SamplerWrapMode::ClampToEdge,
            .max_anisotropy = 0.0f,
            .max_lod = 0.0f,
            .compare_enable = false,
            .compare_op = rhi::CompareOp::Never,
        };
        const auto temp_sampler = rm_->create_sampler(sampler_desc, "BC6H Src Sampler");
        deferred.emplace_back([temp_sampler, rm = rm_] { rm->destroy_sampler(temp_sampler); });
        const VkSampler vk_sampler = rm_->get_sampler(temp_sampler).sampler;

        // --- Create staging buffer sized for the largest base face across all inputs ---
        uint32_t max_blocks_w = 0;
        uint32_t max_blocks_h = 0;
        for (const auto &[handle_ptr, name]: handles) {
            const auto &img = rm_->get_image(*handle_ptr);
            max_blocks_w = std::max(max_blocks_w, (img.desc.width + 3) / 4);
            max_blocks_h = std::max(max_blocks_h, (img.desc.height + 3) / 4);
        }
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

        const rhi::CommandBuffer cmd(ctx.immediate_command_buffer);

        // --- Compress each cubemap ---
        for (const auto &[handle_ptr, debug_name]: handles) {
            // Capture source properties before create_image (which may reallocate the pool).
            const auto &src_img = rm_->get_image(*handle_ptr);
            const uint32_t face_w = src_img.desc.width;
            const uint32_t face_h = src_img.desc.height;
            const uint32_t mip_count = src_img.desc.mip_levels;
            const rhi::Format src_format = src_img.desc.format;
            const uint32_t src_bpp = rhi::format_bytes_per_block(src_format);
            const VkImage src_vk = src_img.image;

            // Create BC6H destination cubemap
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
            auto bc6h_handle = rm_->create_image(bc6h_desc, debug_name);

            // Transition BC6H image UNDEFINED → TRANSFER_DST
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

            cmd.bind_compute_pipeline(pipeline);

            // Per face × per mip: create view, dispatch, barrier, copy
            for (uint32_t mip = 0; mip < mip_count; ++mip) {
                const uint32_t mip_w = std::max(1u, face_w >> mip);
                const uint32_t mip_h = std::max(1u, face_h >> mip);
                const uint32_t blocks_w = (mip_w + 3) / 4;
                const uint32_t blocks_h = (mip_h + 3) / 4;
                const VkDeviceSize mip_buf_size = static_cast<VkDeviceSize>(blocks_w) * blocks_h * 16;

                for (uint32_t face = 0; face < 6; ++face) {
                    constexpr uint32_t kGroupSize = 8;

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

                    struct {
                        float texel_size_rcp[2];
                        uint32_t blocks_per_row;
                        uint32_t blocks_per_col;
                    } pc{};
                    pc.texel_size_rcp[0] = 1.0f / static_cast<float>(mip_w);
                    pc.texel_size_rcp[1] = 1.0f / static_cast<float>(mip_h);
                    pc.blocks_per_row = blocks_w;
                    pc.blocks_per_col = blocks_h;
                    cmd.push_constants(pipeline.layout, VK_SHADER_STAGE_COMPUTE_BIT,
                                       &pc, sizeof(pc));

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

                    // Barrier: transfer read → compute write (reuse SSBO next iteration)
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

            // Transition BC6H cubemap TRANSFER_DST → SHADER_READ_ONLY
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

            VkDependencyInfo final_dep{};
            final_dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
            final_dep.imageMemoryBarrierCount = 1;
            final_dep.pImageMemoryBarriers = &dst_to_read;
            cmd.pipeline_barrier(final_dep);

            // Swap handles: BC6H replaces uncompressed
            auto old_handle = *handle_ptr;
            deferred.emplace_back([this, old_handle] { rm_->destroy_image(old_handle); });
            *handle_ptr = bc6h_handle;

            // Log compression ratio
            uint64_t uncompressed_bytes = 0;
            uint64_t compressed_bytes = 0;
            for (uint32_t m = 0; m < mip_count; ++m) {
                const uint32_t mw = std::max(1u, face_w >> m);
                const uint32_t mh = std::max(1u, face_h >> m);
                uncompressed_bytes += static_cast<uint64_t>(mw) * mh * 6 * src_bpp;
                compressed_bytes += static_cast<uint64_t>((mw + 3) / 4) * ((mh + 3) / 4) * 6 * 16;
            }

            spdlog::info("IBL: BC6H compressed {} {}x{} ({} mips): {:.1f} MB -> {:.1f} MB",
                         debug_name, face_w, face_h, mip_count,
                         static_cast<double>(uncompressed_bytes) / (1024.0 * 1024.0),
                         static_cast<double>(compressed_bytes) / (1024.0 * 1024.0));
        }
    }
} // namespace himalaya::framework
