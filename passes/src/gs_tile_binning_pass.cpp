#include <himalaya/passes/gs_tile_binning_pass.h>

/**
 * @file gs_tile_binning_pass.cpp
 * @brief GsTileBinningPass implementation.
 */

#include <himalaya/framework/frame_context.h>
#include <himalaya/rhi/commands.h>
#include <himalaya/rhi/context.h>
#include <himalaya/rhi/descriptors.h>
#include <himalaya/rhi/resources.h>
#include <himalaya/rhi/shader.h>

#include <array>
#include <span>

#include <spdlog/spdlog.h>

namespace himalaya::passes {
    namespace {
        /**
         * @brief Creates one storage-buffer descriptor layout binding.
         */
        constexpr VkDescriptorSetLayoutBinding storage_binding(const uint32_t binding) {
            return VkDescriptorSetLayoutBinding{
                .binding = binding,
                .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                .descriptorCount = 1,
                .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
            };
        }

        /**
         * @brief Creates a push descriptor layout from storage-buffer bindings.
         */
        VkDescriptorSetLayout create_push_storage_layout(
            rhi::Context &ctx,
            const std::span<const VkDescriptorSetLayoutBinding> bindings) {
            VkDescriptorSetLayoutCreateInfo layout_ci{};
            layout_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
            layout_ci.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT;
            layout_ci.bindingCount = static_cast<uint32_t>(bindings.size());
            layout_ci.pBindings = bindings.data();

            VkDescriptorSetLayout layout = VK_NULL_HANDLE;
            VK_CHECK(vkCreateDescriptorSetLayout(ctx.device, &layout_ci, nullptr, &layout));
            return layout;
        }

        /**
         * @brief Resolves a BufferHandle to a full-range descriptor info.
         */
        VkDescriptorBufferInfo buffer_info(const rhi::ResourceManager &rm, const rhi::BufferHandle handle) {
            const auto &buffer = rm.get_buffer(handle);
            return VkDescriptorBufferInfo{
                .buffer = buffer.buffer,
                .offset = 0,
                .range = buffer.desc.size,
            };
        }

        /**
         * @brief Creates one compute pipeline from a shader file and Set 3 layout.
         */
        rhi::Pipeline create_tile_pipeline(rhi::Context &ctx,
                                           rhi::DescriptorManager &dm,
                                           rhi::ShaderCompiler &sc,
                                           const char *shader_path,
                                           const VkDescriptorSetLayout set3_layout,
                                           const std::span<const VkPushConstantRange> push_ranges) {
            const auto spirv = sc.compile_from_file(shader_path, rhi::ShaderStage::Compute);
            if (spirv.empty()) {
                return {};
            }

            const auto shader_module = rhi::create_shader_module(ctx.device, spirv);
            const auto set_layouts = dm.get_dispatch_set_layouts(set3_layout);
            const rhi::ComputePipelineDesc desc{
                .compute_shader = shader_module,
                .descriptor_set_layouts = set_layouts,
                .push_constant_ranges = {push_ranges.begin(), push_ranges.end()},
            };
            auto pipeline = rhi::create_compute_pipeline(ctx.device, desc);
            vkDestroyShaderModule(ctx.device, shader_module, nullptr);

            return pipeline;
        }
    } // namespace

    void GsTileBinningPass::setup(rhi::Context &ctx,
                                  rhi::ResourceManager &rm,
                                  rhi::DescriptorManager &dm,
                                  rhi::ShaderCompiler &sc) {
        ctx_ = &ctx;
        rm_ = &rm;
        dm_ = &dm;
        sc_ = &sc;

        tile_buffers_.setup(rm);
        create_descriptor_layouts();
        create_pipelines();
    }

    void GsTileBinningPass::ensure_capacity(const uint32_t max_splat_count,
                                            const uint32_t screen_width,
                                            const uint32_t screen_height) {
        tile_buffers_.ensure_capacity(max_splat_count, screen_width, screen_height);
    }

    void GsTileBinningPass::record(const rhi::CommandBuffer &cmd,
                                   const framework::FrameContext &frame_ctx,
                                   const rhi::BufferHandle projected_splat_buffer,
                                   const rhi::BufferHandle sorted_splat_index_buffer,
                                   const rhi::BufferHandle visible_counter_buffer,
                                   const rhi::BufferHandle indirect_dispatch_buffer,
                                   const uint32_t max_splat_count,
                                   const uint32_t screen_width,
                                   const uint32_t screen_height) {
        if (max_splat_count == 0 ||
            count_pipeline_.pipeline == VK_NULL_HANDLE ||
            scan_pipeline_.pipeline == VK_NULL_HANDLE ||
            scatter_pipeline_.pipeline == VK_NULL_HANDLE) {
            return;
        }

        ensure_capacity(max_splat_count, screen_width, screen_height);
        if (!tile_buffers_.tile_counts_buffer().valid() ||
            !tile_buffers_.tile_splat_ids_buffer().valid() ||
            tile_buffers_.tile_count() == 0) {
            return;
        }

        const auto bind_global_sets = [&](const rhi::Pipeline &pipeline) {
            const std::array sets = {
                dm_->get_set0(frame_ctx.frame_index),
                dm_->get_set1(),
                dm_->get_set2(frame_ctx.frame_index),
            };
            cmd.bind_compute_descriptor_sets(pipeline.layout, 0, sets.data(), static_cast<uint32_t>(sets.size()));
        };

        const auto push_buffers = [&](const rhi::Pipeline &pipeline,
                                      const std::span<const VkDescriptorBufferInfo> infos) {
            std::array<VkWriteDescriptorSet, 8> writes{};
            for (uint32_t i = 0; i < infos.size(); ++i) {
                writes[i] = VkWriteDescriptorSet{
                    .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                    .dstBinding = i,
                    .descriptorCount = 1,
                    .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                    .pBufferInfo = &infos[i],
                };
            }
            cmd.push_compute_descriptor_set(pipeline.layout, 3,
                                            std::span<const VkWriteDescriptorSet>(writes.data(), infos.size()));
        };

        const auto storage_read = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
        const auto storage_write = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        const auto transfer_write = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        const auto compute_stage = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        const auto transfer_stage = VK_PIPELINE_STAGE_2_TRANSFER_BIT;

        const auto &tile_counts = rm_->get_buffer(tile_buffers_.tile_counts_buffer());
        const auto &tile_cursors = rm_->get_buffer(tile_buffers_.tile_cursors_buffer());
        const auto &tile_chunk_sums = rm_->get_buffer(tile_buffers_.tile_scan_chunk_sums_buffer());
        const auto &tile_total_count = rm_->get_buffer(tile_buffers_.tile_total_count_buffer());
        const auto &indirect_dispatch = rm_->get_buffer(indirect_dispatch_buffer);

        vkCmdFillBuffer(cmd.handle(), tile_counts.buffer, 0, tile_counts.desc.size, 0u);
        vkCmdFillBuffer(cmd.handle(), tile_chunk_sums.buffer, 0, tile_chunk_sums.desc.size, 0u);
        vkCmdFillBuffer(cmd.handle(), tile_total_count.buffer, 0, tile_total_count.desc.size, 0u);
        buffer_barrier(cmd,
                       tile_buffers_.tile_counts_buffer(),
                       transfer_stage,
                       transfer_write,
                       compute_stage,
                       storage_read | storage_write);
        buffer_barrier(cmd,
                       tile_buffers_.tile_scan_chunk_sums_buffer(),
                       transfer_stage,
                       transfer_write,
                       compute_stage,
                       storage_read | storage_write);
        buffer_barrier(cmd,
                       tile_buffers_.tile_total_count_buffer(),
                       transfer_stage,
                       transfer_write,
                       compute_stage,
                       storage_read | storage_write);

        const auto projected_info = buffer_info(*rm_, projected_splat_buffer);
        const auto sorted_splat_index_info = buffer_info(*rm_, sorted_splat_index_buffer);
        const auto visible_counter_info = buffer_info(*rm_, visible_counter_buffer);
        const auto tile_offsets_info = buffer_info(*rm_, tile_buffers_.tile_offsets_buffer());
        const auto tile_counts_info = buffer_info(*rm_, tile_buffers_.tile_counts_buffer());
        const auto tile_cursors_info = buffer_info(*rm_, tile_buffers_.tile_cursors_buffer());
        const auto tile_chunk_sums_info = buffer_info(*rm_, tile_buffers_.tile_scan_chunk_sums_buffer());
        const auto tile_total_count_info = buffer_info(*rm_, tile_buffers_.tile_total_count_buffer());
        const auto tile_splat_ids_info = buffer_info(*rm_, tile_buffers_.tile_splat_ids_buffer());

        cmd.bind_compute_pipeline(count_pipeline_);
        bind_global_sets(count_pipeline_);
        {
            const std::array infos = {
                projected_info,
                sorted_splat_index_info,
                visible_counter_info,
                tile_counts_info,
            };
            push_buffers(count_pipeline_, infos);
            const CountPushConstants pc{
                .max_splat_count = max_splat_count,
                .tile_count_x = tile_buffers_.tile_count_x(),
                .tile_count_y = tile_buffers_.tile_count_y(),
                ._padding = 0,
            };
            cmd.push_constants(count_pipeline_.layout, VK_SHADER_STAGE_COMPUTE_BIT, &pc, sizeof(pc));
            vkCmdDispatchIndirect(cmd.handle(), indirect_dispatch.buffer, 0);
        }
        buffer_barrier(cmd,
                       tile_buffers_.tile_counts_buffer(),
                       compute_stage,
                       storage_write,
                       compute_stage,
                       storage_read | storage_write);

        cmd.bind_compute_pipeline(scan_pipeline_);
        bind_global_sets(scan_pipeline_);
        {
            const std::array infos = {
                tile_counts_info,
                tile_offsets_info,
                tile_chunk_sums_info,
                tile_total_count_info,
            };
            push_buffers(scan_pipeline_, infos);

            ScanPushConstants pc{
                .mode = static_cast<uint32_t>(ScanMode::ScanTileChunks),
                .tile_count = tile_buffers_.tile_count(),
                .chunk_count = tile_buffers_.scan_chunk_count(),
                ._padding = 0,
            };
            cmd.push_constants(scan_pipeline_.layout, VK_SHADER_STAGE_COMPUTE_BIT, &pc, sizeof(pc));
            cmd.dispatch(tile_buffers_.scan_chunk_count(), 1, 1);
            buffer_barrier(cmd,
                           tile_buffers_.tile_scan_chunk_sums_buffer(),
                           compute_stage,
                           storage_write,
                           compute_stage,
                           storage_read | storage_write);

            pc.mode = static_cast<uint32_t>(ScanMode::ScanChunkSums);
            cmd.push_constants(scan_pipeline_.layout, VK_SHADER_STAGE_COMPUTE_BIT, &pc, sizeof(pc));
            cmd.dispatch(1, 1, 1);
            buffer_barrier(cmd,
                           tile_buffers_.tile_scan_chunk_sums_buffer(),
                           compute_stage,
                           storage_write,
                           compute_stage,
                           storage_read | storage_write);
            buffer_barrier(cmd,
                           tile_buffers_.tile_total_count_buffer(),
                           compute_stage,
                           storage_write,
                           compute_stage,
                           storage_read | storage_write);

            pc.mode = static_cast<uint32_t>(ScanMode::AddChunkOffsets);
            cmd.push_constants(scan_pipeline_.layout, VK_SHADER_STAGE_COMPUTE_BIT, &pc, sizeof(pc));
            cmd.dispatch(tile_buffers_.scan_chunk_count(), 1, 1);
        }
        buffer_barrier(cmd,
                       tile_buffers_.tile_offsets_buffer(),
                       compute_stage,
                       storage_write,
                       compute_stage,
                       storage_read);

        vkCmdFillBuffer(cmd.handle(), tile_cursors.buffer, 0, tile_cursors.desc.size, 0u);
        buffer_barrier(cmd,
                       tile_buffers_.tile_cursors_buffer(),
                       transfer_stage,
                       transfer_write,
                       compute_stage,
                       storage_read | storage_write);

        cmd.bind_compute_pipeline(scatter_pipeline_);
        bind_global_sets(scatter_pipeline_);
        {
            const std::array infos = {
                projected_info,
                sorted_splat_index_info,
                visible_counter_info,
                tile_offsets_info,
                tile_cursors_info,
                tile_splat_ids_info,
            };
            push_buffers(scatter_pipeline_, infos);
            const ScatterPushConstants pc{
                .max_splat_count = max_splat_count,
                .tile_count_x = tile_buffers_.tile_count_x(),
                .tile_count_y = tile_buffers_.tile_count_y(),
                .tile_splat_id_capacity = tile_buffers_.tile_splat_id_capacity(),
            };
            cmd.push_constants(scatter_pipeline_.layout, VK_SHADER_STAGE_COMPUTE_BIT, &pc, sizeof(pc));
            vkCmdDispatchIndirect(cmd.handle(), indirect_dispatch.buffer, 0);
        }
        barrier_tile_outputs_to_compute_read(cmd);
    }

    void GsTileBinningPass::rebuild_pipelines() {
        create_pipelines();
    }

    void GsTileBinningPass::destroy() {
        tile_buffers_.destroy();
        destroy_pipelines();

        if (count_set3_layout_ != VK_NULL_HANDLE) {
            vkDestroyDescriptorSetLayout(ctx_->device, count_set3_layout_, nullptr);
            count_set3_layout_ = VK_NULL_HANDLE;
        }
        if (scan_set3_layout_ != VK_NULL_HANDLE) {
            vkDestroyDescriptorSetLayout(ctx_->device, scan_set3_layout_, nullptr);
            scan_set3_layout_ = VK_NULL_HANDLE;
        }
        if (scatter_set3_layout_ != VK_NULL_HANDLE) {
            vkDestroyDescriptorSetLayout(ctx_->device, scatter_set3_layout_, nullptr);
            scatter_set3_layout_ = VK_NULL_HANDLE;
        }
    }

    const GsTileBuffers &GsTileBinningPass::tile_buffers() const {
        return tile_buffers_;
    }

    void GsTileBinningPass::create_descriptor_layouts() {
        const std::array count_bindings = {
            storage_binding(0),
            storage_binding(1),
            storage_binding(2),
            storage_binding(3),
        };
        count_set3_layout_ = create_push_storage_layout(*ctx_, count_bindings);

        const std::array scan_bindings = {
            storage_binding(0),
            storage_binding(1),
            storage_binding(2),
            storage_binding(3),
        };
        scan_set3_layout_ = create_push_storage_layout(*ctx_, scan_bindings);

        const std::array scatter_bindings = {
            storage_binding(0),
            storage_binding(1),
            storage_binding(2),
            storage_binding(3),
            storage_binding(4),
            storage_binding(5),
        };
        scatter_set3_layout_ = create_push_storage_layout(*ctx_, scatter_bindings);
    }

    void GsTileBinningPass::create_pipelines() {
        const std::array count_push_ranges = {
            VkPushConstantRange{
                .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
                .offset = 0,
                .size = sizeof(CountPushConstants),
            },
        };
        const std::array scan_push_ranges = {
            VkPushConstantRange{
                .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
                .offset = 0,
                .size = sizeof(ScanPushConstants),
            },
        };
        const std::array scatter_push_ranges = {
            VkPushConstantRange{
                .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
                .offset = 0,
                .size = sizeof(ScatterPushConstants),
            },
        };

        auto count_pipeline = create_tile_pipeline(*ctx_, *dm_, *sc_,
                                                   "gs/gs_tile_count.comp",
                                                   count_set3_layout_,
                                                   count_push_ranges);
        auto scan_pipeline = create_tile_pipeline(*ctx_, *dm_, *sc_,
                                                  "gs/gs_tile_scan.comp",
                                                  scan_set3_layout_,
                                                  scan_push_ranges);
        auto scatter_pipeline = create_tile_pipeline(*ctx_, *dm_, *sc_,
                                                     "gs/gs_tile_scatter.comp",
                                                     scatter_set3_layout_,
                                                     scatter_push_ranges);

        if (count_pipeline.pipeline == VK_NULL_HANDLE ||
            scan_pipeline.pipeline == VK_NULL_HANDLE ||
            scatter_pipeline.pipeline == VK_NULL_HANDLE) {
            if (count_pipeline.pipeline != VK_NULL_HANDLE) {
                count_pipeline.destroy(ctx_->device);
            }
            if (scan_pipeline.pipeline != VK_NULL_HANDLE) {
                scan_pipeline.destroy(ctx_->device);
            }
            if (scatter_pipeline.pipeline != VK_NULL_HANDLE) {
                scatter_pipeline.destroy(ctx_->device);
            }
            spdlog::warn("GsTileBinningPass: shader compilation failed, keeping previous pipelines");
            return;
        }

        destroy_pipelines();
        count_pipeline_ = count_pipeline;
        scan_pipeline_ = scan_pipeline;
        scatter_pipeline_ = scatter_pipeline;
    }

    void GsTileBinningPass::destroy_pipelines() {
        if (count_pipeline_.pipeline != VK_NULL_HANDLE) {
            count_pipeline_.destroy(ctx_->device);
            count_pipeline_ = {};
        }
        if (scan_pipeline_.pipeline != VK_NULL_HANDLE) {
            scan_pipeline_.destroy(ctx_->device);
            scan_pipeline_ = {};
        }
        if (scatter_pipeline_.pipeline != VK_NULL_HANDLE) {
            scatter_pipeline_.destroy(ctx_->device);
            scatter_pipeline_ = {};
        }
    }

    void GsTileBinningPass::buffer_barrier(const rhi::CommandBuffer &cmd,
                                           const rhi::BufferHandle buffer,
                                           const VkPipelineStageFlags2 src_stage,
                                           const VkAccessFlags2 src_access,
                                           const VkPipelineStageFlags2 dst_stage,
                                           const VkAccessFlags2 dst_access) const {
        if (!buffer.valid()) {
            return;
        }

        const auto &resolved = rm_->get_buffer(buffer);
        VkBufferMemoryBarrier2 barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
        barrier.srcStageMask = src_stage;
        barrier.srcAccessMask = src_access;
        barrier.dstStageMask = dst_stage;
        barrier.dstAccessMask = dst_access;
        barrier.buffer = resolved.buffer;
        barrier.offset = 0;
        barrier.size = resolved.desc.size;

        VkDependencyInfo dep{};
        dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dep.bufferMemoryBarrierCount = 1;
        dep.pBufferMemoryBarriers = &barrier;
        cmd.pipeline_barrier(dep);
    }

    void GsTileBinningPass::barrier_tile_outputs_to_compute_read(const rhi::CommandBuffer &cmd) const {
        const auto compute_stage = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        const auto storage_read = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
        const auto storage_write = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;

        buffer_barrier(cmd,
                       tile_buffers_.tile_counts_buffer(),
                       compute_stage,
                       storage_write,
                       compute_stage,
                       storage_read);
        buffer_barrier(cmd,
                       tile_buffers_.tile_offsets_buffer(),
                       compute_stage,
                       storage_write,
                       compute_stage,
                       storage_read);
        buffer_barrier(cmd,
                       tile_buffers_.tile_cursors_buffer(),
                       compute_stage,
                       storage_write,
                       compute_stage,
                       storage_read);
        buffer_barrier(cmd,
                       tile_buffers_.tile_splat_ids_buffer(),
                       compute_stage,
                       storage_write,
                       compute_stage,
                       storage_read);
    }
} // namespace himalaya::passes
