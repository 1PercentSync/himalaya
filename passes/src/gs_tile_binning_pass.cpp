#include <himalaya/passes/gs_tile_binning_pass.h>

/**
 * @file gs_tile_binning_pass.cpp
 * @brief GsTileBinningPass implementation.
 */

#include <himalaya/framework/frame_context.h>
#include <himalaya/rhi/commands.h>
#include <himalaya/rhi/compute_utils.h>
#include <himalaya/rhi/context.h>
#include <himalaya/rhi/descriptors.h>
#include <himalaya/rhi/resources.h>

#include <array>
#include <cstddef>
#include <cstring>

#include <spdlog/spdlog.h>

namespace himalaya::passes {
    void GsTileBinningPass::setup(rhi::Context &ctx,
                                  rhi::ResourceManager &rm,
                                  rhi::DescriptorManager &dm,
                                  rhi::ShaderCompiler &sc) {
        ctx_ = &ctx;
        rm_ = &rm;
        dm_ = &dm;
        sc_ = &sc;

        tile_buffers_.setup(rm);
        for (uint32_t i = 0; i < rhi::kMaxFramesInFlight; ++i) {
            stats_readback_buffers_[i] = rm_->create_buffer({
                .size = sizeof(GsRuntimeStats),
                .usage = rhi::BufferUsage::TransferDst,
                .memory = rhi::MemoryUsage::GpuToCpu,
            }, "GS Runtime Stats Readback");
        }

        depth_sorter_.setup(ctx, rm, dm, sc);
        tile_sorter_.setup(ctx, rm, dm, sc);
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
                                   const rhi::BufferHandle depth_key_buffer,
                                   const rhi::BufferHandle visible_counter_buffer,
                                   const rhi::BufferHandle indirect_dispatch_buffer,
                                   const uint32_t max_splat_count,
                                   const uint32_t screen_width,
                                   const uint32_t screen_height) {
        consume_delayed_stats(frame_ctx.frame_index);

        if (max_splat_count == 0 ||
            !projected_splat_buffer.valid() ||
            !depth_key_buffer.valid() ||
            !visible_counter_buffer.valid() ||
            !indirect_dispatch_buffer.valid() ||
            entry_pipeline_.pipeline == VK_NULL_HANDLE ||
            gather_pipeline_.pipeline == VK_NULL_HANDLE ||
            range_pipeline_.pipeline == VK_NULL_HANDLE) {
            return;
        }

        ensure_capacity(max_splat_count, screen_width, screen_height);
        if (!tile_buffers_.entry_depth_keys_buffer().valid() ||
            !tile_buffers_.entry_count_buffer().valid() ||
            !tile_buffers_.entry_stats_buffer().valid() ||
            tile_buffers_.tile_count() == 0 ||
            tile_buffers_.entry_capacity() == 0) {
            return;
        }

        const auto storage_read = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
        const auto storage_write = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        const auto transfer_write = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        const auto compute_stage = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        const auto transfer_stage = VK_PIPELINE_STAGE_2_TRANSFER_BIT;

        const auto &entry_count = rm_->get_buffer(tile_buffers_.entry_count_buffer());
        const auto &entry_stats = rm_->get_buffer(tile_buffers_.entry_stats_buffer());
        const auto &tile_offsets = rm_->get_buffer(tile_buffers_.tile_offsets_buffer());
        const auto &tile_counts = rm_->get_buffer(tile_buffers_.tile_counts_buffer());
        const auto &indirect_dispatch = rm_->get_buffer(indirect_dispatch_buffer);

        vkCmdFillBuffer(cmd.handle(), entry_count.buffer, 0, entry_count.desc.size, 0u);
        vkCmdFillBuffer(cmd.handle(), entry_stats.buffer, 0, entry_stats.desc.size, 0u);
        vkCmdFillBuffer(cmd.handle(), tile_offsets.buffer, 0, tile_offsets.desc.size, 0u);
        vkCmdFillBuffer(cmd.handle(), tile_counts.buffer, 0, tile_counts.desc.size, 0u);
        rhi::buffer_barrier(cmd,
                            *rm_,
                            visible_counter_buffer,
                            compute_stage,
                            storage_write,
                            transfer_stage,
                            VK_ACCESS_2_TRANSFER_READ_BIT);
        {
            VkBufferCopy visible_copy{};
            visible_copy.srcOffset = 0;
            visible_copy.dstOffset = offsetof(GsRuntimeStats, visible_splats);
            visible_copy.size = sizeof(uint32_t);
            vkCmdCopyBuffer(cmd.handle(),
                            rm_->get_buffer(visible_counter_buffer).buffer,
                            entry_stats.buffer,
                            1,
                            &visible_copy);
        }
        rhi::buffer_barrier(cmd,
                            *rm_,
                            visible_counter_buffer,
                            transfer_stage,
                            VK_ACCESS_2_TRANSFER_READ_BIT,
                            compute_stage,
                            storage_read);
        rhi::buffer_barrier(cmd,
                            *rm_,
                            tile_buffers_.entry_count_buffer(),
                            transfer_stage,
                            transfer_write,
                            compute_stage,
                            storage_read | storage_write);
        rhi::buffer_barrier(cmd,
                            *rm_,
                            tile_buffers_.entry_stats_buffer(),
                            transfer_stage,
                            transfer_write,
                            compute_stage,
                            storage_read | storage_write);
        rhi::buffer_barrier(cmd,
                            *rm_,
                            tile_buffers_.tile_offsets_buffer(),
                            transfer_stage,
                            transfer_write,
                            compute_stage,
                            storage_read | storage_write);
        rhi::buffer_barrier(cmd,
                            *rm_,
                            tile_buffers_.tile_counts_buffer(),
                            transfer_stage,
                            transfer_write,
                            compute_stage,
                            storage_read | storage_write);

        const auto projected_info = rhi::storage_buffer_info(*rm_, projected_splat_buffer);
        const auto depth_key_info = rhi::storage_buffer_info(*rm_, depth_key_buffer);
        const auto visible_counter_info = rhi::storage_buffer_info(*rm_, visible_counter_buffer);
        const auto entry_count_info = rhi::storage_buffer_info(*rm_, tile_buffers_.entry_count_buffer());
        const auto entry_stats_info = rhi::storage_buffer_info(*rm_, tile_buffers_.entry_stats_buffer());
        const auto entry_depth_keys_info = rhi::storage_buffer_info(*rm_, tile_buffers_.entry_depth_keys_buffer());
        const auto entry_tile_ids_info = rhi::storage_buffer_info(*rm_, tile_buffers_.entry_tile_ids_buffer());
        const auto entry_splat_ids_info = rhi::storage_buffer_info(*rm_, tile_buffers_.entry_splat_ids_buffer());
        const auto entry_indices_info = rhi::storage_buffer_info(*rm_, tile_buffers_.entry_indices_buffer());
        const auto tile_sort_keys_info = rhi::storage_buffer_info(*rm_, tile_buffers_.tile_sort_keys_buffer());
        const auto tile_sort_values_info = rhi::storage_buffer_info(*rm_, tile_buffers_.tile_sort_values_buffer());
        const auto tile_offsets_info = rhi::storage_buffer_info(*rm_, tile_buffers_.tile_offsets_buffer());
        const auto tile_counts_info = rhi::storage_buffer_info(*rm_, tile_buffers_.tile_counts_buffer());

        cmd.bind_compute_pipeline(entry_pipeline_);
        rhi::bind_dispatch_descriptor_sets(cmd, *dm_, entry_pipeline_, frame_ctx.frame_index);
        {
            const std::array infos = {
                projected_info,
                depth_key_info,
                visible_counter_info,
                entry_count_info,
                entry_stats_info,
                entry_depth_keys_info,
                entry_tile_ids_info,
                entry_splat_ids_info,
                entry_indices_info,
            };
            rhi::push_storage_buffers(cmd, entry_pipeline_, infos);
            const EntryPushConstants pc{
                .max_splat_count = max_splat_count,
                .entry_capacity = tile_buffers_.entry_capacity(),
                .tile_count_x = tile_buffers_.tile_count_x(),
                .tile_count_y = tile_buffers_.tile_count_y(),
            };
            cmd.push_constants(entry_pipeline_.layout, VK_SHADER_STAGE_COMPUTE_BIT, &pc, sizeof(pc));
            const uint32_t group_count = (max_splat_count + kWorkgroupSize - 1u) / kWorkgroupSize;
            cmd.dispatch(group_count, 1, 1);
        }
        barrier_entry_outputs_to_compute_read(cmd);

        depth_sorter_.record(cmd,
                             frame_ctx,
                             tile_buffers_.entry_depth_keys_buffer(),
                             tile_buffers_.entry_indices_buffer(),
                             tile_buffers_.entry_count_buffer(),
                             indirect_dispatch_buffer,
                             tile_buffers_.entry_capacity());
        const auto depth_sorted_entries = depth_sorter_.sorted_value_buffer();
        if (!depth_sorted_entries.valid()) {
            return;
        }
        const auto depth_sorted_entries_info = rhi::storage_buffer_info(*rm_, depth_sorted_entries);

        cmd.bind_compute_pipeline(gather_pipeline_);
        rhi::bind_dispatch_descriptor_sets(cmd, *dm_, gather_pipeline_, frame_ctx.frame_index);
        {
            const std::array infos = {
                entry_tile_ids_info,
                depth_sorted_entries_info,
                entry_count_info,
                tile_sort_keys_info,
                tile_sort_values_info,
            };
            rhi::push_storage_buffers(cmd, gather_pipeline_, infos);
            const GatherPushConstants pc{
                .max_entry_count = tile_buffers_.entry_capacity(),
            };
            cmd.push_constants(gather_pipeline_.layout, VK_SHADER_STAGE_COMPUTE_BIT, &pc, sizeof(pc));
            vkCmdDispatchIndirect(cmd.handle(), indirect_dispatch.buffer, 0);
        }
        barrier_gather_outputs_to_compute_read(cmd);

        tile_sorter_.record(cmd,
                            frame_ctx,
                            tile_buffers_.tile_sort_keys_buffer(),
                            tile_buffers_.tile_sort_values_buffer(),
                            tile_buffers_.entry_count_buffer(),
                            indirect_dispatch_buffer,
                            tile_buffers_.entry_capacity());
        if (!tile_sorter_.sorted_key_buffer().valid() || !tile_sorter_.sorted_value_buffer().valid()) {
            return;
        }

        const auto sorted_tile_ids_info = rhi::storage_buffer_info(*rm_, tile_sorter_.sorted_key_buffer());
        cmd.bind_compute_pipeline(range_pipeline_);
        rhi::bind_dispatch_descriptor_sets(cmd, *dm_, range_pipeline_, frame_ctx.frame_index);
        {
            const std::array infos = {
                sorted_tile_ids_info,
                entry_count_info,
                tile_offsets_info,
                tile_counts_info,
                entry_stats_info,
            };
            rhi::push_storage_buffers(cmd, range_pipeline_, infos);
            const RangePushConstants pc{
                .max_entry_count = tile_buffers_.entry_capacity(),
                .tile_count = tile_buffers_.tile_count(),
            };
            cmd.push_constants(range_pipeline_.layout, VK_SHADER_STAGE_COMPUTE_BIT, &pc, sizeof(pc));
            vkCmdDispatchIndirect(cmd.handle(), indirect_dispatch.buffer, 0);
        }
        barrier_range_outputs_to_compute_read(cmd);
        record_stats_readback(cmd, frame_ctx.frame_index);
    }

    void GsTileBinningPass::on_resize(const uint32_t screen_width, const uint32_t screen_height) {
        if (tile_buffers_.max_splat_count() == 0) {
            return;
        }
        tile_buffers_.ensure_capacity(tile_buffers_.max_splat_count(), screen_width, screen_height);
    }

    void GsTileBinningPass::rebuild_pipelines() {
        create_pipelines();
        depth_sorter_.rebuild_pipelines();
        tile_sorter_.rebuild_pipelines();
    }

    void GsTileBinningPass::destroy() {
        tile_buffers_.destroy();
        destroy_readback_buffers();
        depth_sorter_.destroy();
        tile_sorter_.destroy();
        destroy_pipelines();

        if (entry_set3_layout_ != VK_NULL_HANDLE) {
            vkDestroyDescriptorSetLayout(ctx_->device, entry_set3_layout_, nullptr);
            entry_set3_layout_ = VK_NULL_HANDLE;
        }
        if (gather_set3_layout_ != VK_NULL_HANDLE) {
            vkDestroyDescriptorSetLayout(ctx_->device, gather_set3_layout_, nullptr);
            gather_set3_layout_ = VK_NULL_HANDLE;
        }
        if (range_set3_layout_ != VK_NULL_HANDLE) {
            vkDestroyDescriptorSetLayout(ctx_->device, range_set3_layout_, nullptr);
            range_set3_layout_ = VK_NULL_HANDLE;
        }
    }

    const GsTileBuffers &GsTileBinningPass::tile_buffers() const {
        return tile_buffers_;
    }

    rhi::BufferHandle GsTileBinningPass::sorted_tile_ids_buffer() const {
        return tile_sorter_.sorted_key_buffer();
    }

    rhi::BufferHandle GsTileBinningPass::sorted_entry_indices_buffer() const {
        return tile_sorter_.sorted_value_buffer();
    }

    bool GsTileBinningPass::has_runtime_stats() const {
        return has_runtime_stats_;
    }

    const GsRuntimeStats &GsTileBinningPass::runtime_stats() const {
        return runtime_stats_;
    }

    void GsTileBinningPass::create_descriptor_layouts() {
        const std::array entry_bindings = {
            rhi::storage_buffer_binding(0),
            rhi::storage_buffer_binding(1),
            rhi::storage_buffer_binding(2),
            rhi::storage_buffer_binding(3),
            rhi::storage_buffer_binding(4),
            rhi::storage_buffer_binding(5),
            rhi::storage_buffer_binding(6),
            rhi::storage_buffer_binding(7),
            rhi::storage_buffer_binding(8),
        };
        entry_set3_layout_ = rhi::create_push_storage_descriptor_set_layout(*ctx_, entry_bindings);

        const std::array gather_bindings = {
            rhi::storage_buffer_binding(0),
            rhi::storage_buffer_binding(1),
            rhi::storage_buffer_binding(2),
            rhi::storage_buffer_binding(3),
            rhi::storage_buffer_binding(4),
        };
        gather_set3_layout_ = rhi::create_push_storage_descriptor_set_layout(*ctx_, gather_bindings);

        const std::array range_bindings = {
            rhi::storage_buffer_binding(0),
            rhi::storage_buffer_binding(1),
            rhi::storage_buffer_binding(2),
            rhi::storage_buffer_binding(3),
            rhi::storage_buffer_binding(4),
        };
        range_set3_layout_ = rhi::create_push_storage_descriptor_set_layout(*ctx_, range_bindings);
    }

    void GsTileBinningPass::create_pipelines() {
        const std::array entry_push_ranges = {
            VkPushConstantRange{
                .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
                .offset = 0,
                .size = sizeof(EntryPushConstants),
            },
        };
        const std::array gather_push_ranges = {
            VkPushConstantRange{
                .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
                .offset = 0,
                .size = sizeof(GatherPushConstants),
            },
        };
        const std::array range_push_ranges = {
            VkPushConstantRange{
                .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
                .offset = 0,
                .size = sizeof(RangePushConstants),
            },
        };

        auto entry_pipeline = rhi::create_compute_pipeline_from_file(*ctx_, *dm_, *sc_,
                                                                     "gs/gs_tile_entry.comp",
                                                                     entry_set3_layout_,
                                                                     entry_push_ranges);
        auto gather_pipeline = rhi::create_compute_pipeline_from_file(*ctx_, *dm_, *sc_,
                                                                      "gs/gs_tile_sort_gather.comp",
                                                                      gather_set3_layout_,
                                                                      gather_push_ranges);
        auto range_pipeline = rhi::create_compute_pipeline_from_file(*ctx_, *dm_, *sc_,
                                                                     "gs/gs_tile_range.comp",
                                                                     range_set3_layout_,
                                                                     range_push_ranges);

        if (entry_pipeline.pipeline == VK_NULL_HANDLE ||
            gather_pipeline.pipeline == VK_NULL_HANDLE ||
            range_pipeline.pipeline == VK_NULL_HANDLE) {
            if (entry_pipeline.pipeline != VK_NULL_HANDLE) {
                entry_pipeline.destroy(ctx_->device);
            }
            if (gather_pipeline.pipeline != VK_NULL_HANDLE) {
                gather_pipeline.destroy(ctx_->device);
            }
            if (range_pipeline.pipeline != VK_NULL_HANDLE) {
                range_pipeline.destroy(ctx_->device);
            }
            spdlog::warn("GsTileBinningPass: shader compilation failed, keeping previous pipelines");
            return;
        }

        destroy_pipelines();
        entry_pipeline_ = entry_pipeline;
        gather_pipeline_ = gather_pipeline;
        range_pipeline_ = range_pipeline;
    }

    void GsTileBinningPass::destroy_pipelines() {
        if (entry_pipeline_.pipeline != VK_NULL_HANDLE) {
            entry_pipeline_.destroy(ctx_->device);
            entry_pipeline_ = {};
        }
        if (gather_pipeline_.pipeline != VK_NULL_HANDLE) {
            gather_pipeline_.destroy(ctx_->device);
            gather_pipeline_ = {};
        }
        if (range_pipeline_.pipeline != VK_NULL_HANDLE) {
            range_pipeline_.destroy(ctx_->device);
            range_pipeline_ = {};
        }
    }

    void GsTileBinningPass::barrier_entry_outputs_to_compute_read(const rhi::CommandBuffer &cmd) const {
        const auto compute_stage = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        const auto storage_read = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
        const auto storage_write = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;

        rhi::buffer_barrier(cmd,
                            *rm_,
                            tile_buffers_.entry_count_buffer(),
                            compute_stage,
                            storage_write,
                            compute_stage,
                            storage_read | storage_write);
        rhi::buffer_barrier(cmd,
                            *rm_,
                            tile_buffers_.entry_stats_buffer(),
                            compute_stage,
                            storage_write,
                            compute_stage,
                            storage_read | storage_write);
        rhi::buffer_barrier(cmd,
                            *rm_,
                            tile_buffers_.entry_depth_keys_buffer(),
                            compute_stage,
                            storage_write,
                            compute_stage,
                            storage_read);
        rhi::buffer_barrier(cmd,
                            *rm_,
                            tile_buffers_.entry_tile_ids_buffer(),
                            compute_stage,
                            storage_write,
                            compute_stage,
                            storage_read);
        rhi::buffer_barrier(cmd,
                            *rm_,
                            tile_buffers_.entry_splat_ids_buffer(),
                            compute_stage,
                            storage_write,
                            compute_stage,
                            storage_read);
        rhi::buffer_barrier(cmd,
                            *rm_,
                            tile_buffers_.entry_indices_buffer(),
                            compute_stage,
                            storage_write,
                            compute_stage,
                            storage_read);
    }

    void GsTileBinningPass::barrier_gather_outputs_to_compute_read(const rhi::CommandBuffer &cmd) const {
        const auto compute_stage = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        const auto storage_read = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
        const auto storage_write = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;

        rhi::buffer_barrier(cmd,
                            *rm_,
                            tile_buffers_.tile_sort_keys_buffer(),
                            compute_stage,
                            storage_write,
                            compute_stage,
                            storage_read);
        rhi::buffer_barrier(cmd,
                            *rm_,
                            tile_buffers_.tile_sort_values_buffer(),
                            compute_stage,
                            storage_write,
                            compute_stage,
                            storage_read);
    }

    void GsTileBinningPass::barrier_range_outputs_to_compute_read(const rhi::CommandBuffer &cmd) const {
        const auto compute_stage = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        const auto storage_read = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
        const auto storage_write = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;

        rhi::buffer_barrier(cmd,
                            *rm_,
                            tile_buffers_.tile_offsets_buffer(),
                            compute_stage,
                            storage_write,
                            compute_stage,
                            storage_read);
        rhi::buffer_barrier(cmd,
                            *rm_,
                            tile_buffers_.tile_counts_buffer(),
                            compute_stage,
                            storage_write,
                            compute_stage,
                            storage_read);
        rhi::buffer_barrier(cmd,
                            *rm_,
                            tile_buffers_.entry_stats_buffer(),
                            compute_stage,
                            storage_write,
                            compute_stage,
                            storage_read | storage_write);
    }

    void GsTileBinningPass::consume_delayed_stats(const uint32_t frame_index) {
        if (frame_index >= stats_readback_buffers_.size() ||
            !stats_readback_valid_[frame_index] ||
            !stats_readback_buffers_[frame_index].valid()) {
            return;
        }

        const auto &readback = rm_->get_buffer(stats_readback_buffers_[frame_index]);
        if (readback.allocation_info.pMappedData == nullptr) {
            return;
        }

        VK_CHECK(vmaInvalidateAllocation(ctx_->allocator, readback.allocation, 0, VK_WHOLE_SIZE));
        std::memcpy(&runtime_stats_, readback.allocation_info.pMappedData, sizeof(GsRuntimeStats));
        if (runtime_stats_.visible_splats > tile_buffers_.max_splat_count() ||
            runtime_stats_.entry_written > tile_buffers_.entry_capacity()) {
            runtime_stats_.sort_clamped = 1;
        }
        has_runtime_stats_ = true;
        log_runtime_stats_warnings();
    }

    void GsTileBinningPass::record_stats_readback(const rhi::CommandBuffer &cmd,
                                                  const uint32_t frame_index) {
        if (frame_index >= stats_readback_buffers_.size() ||
            !stats_readback_buffers_[frame_index].valid() ||
            !tile_buffers_.entry_stats_buffer().valid()) {
            return;
        }

        const auto &stats = rm_->get_buffer(tile_buffers_.entry_stats_buffer());
        const auto &readback = rm_->get_buffer(stats_readback_buffers_[frame_index]);

        rhi::buffer_barrier(cmd,
                            *rm_,
                            tile_buffers_.entry_stats_buffer(),
                            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                            VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                            VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                            VK_ACCESS_2_TRANSFER_READ_BIT);

        VkBufferCopy copy{};
        copy.srcOffset = 0;
        copy.dstOffset = 0;
        copy.size = sizeof(GsRuntimeStats);
        vkCmdCopyBuffer(cmd.handle(), stats.buffer, readback.buffer, 1, &copy);

        stats_readback_valid_[frame_index] = true;
    }

    void GsTileBinningPass::destroy_readback_buffers() {
        if (rm_ == nullptr) {
            stats_readback_valid_.fill(false);
            return;
        }

        for (auto &buffer : stats_readback_buffers_) {
            if (buffer.valid()) {
                rm_->destroy_buffer(buffer);
                buffer = {};
            }
        }
        stats_readback_valid_.fill(false);
        has_runtime_stats_ = false;
        runtime_stats_ = {};
    }

    void GsTileBinningPass::log_runtime_stats_warnings() {
        if (!has_runtime_stats_) {
            return;
        }

        if (runtime_stats_.entry_dropped > 0 && !warned_entry_dropped_) {
            spdlog::warn("GS tile entries dropped: requested={}, written={}, dropped={}, capacity={}",
                         runtime_stats_.entry_requested,
                         runtime_stats_.entry_written,
                         runtime_stats_.entry_dropped,
                         tile_buffers_.entry_capacity());
            warned_entry_dropped_ = true;
        }

        if (runtime_stats_.invalid_entries > 0 && !warned_invalid_entries_) {
            spdlog::warn("GS invalid tile entries detected: invalid_entries={}", runtime_stats_.invalid_entries);
            warned_invalid_entries_ = true;
        }

        if (runtime_stats_.sort_clamped > 0 && !warned_sort_clamped_) {
            spdlog::warn("GS sort input was clamped: visible_splats={}, entry_written={}, entry_capacity={}",
                         runtime_stats_.visible_splats,
                         runtime_stats_.entry_written,
                         tile_buffers_.entry_capacity());
            warned_sort_clamped_ = true;
        }
    }
} // namespace himalaya::passes
