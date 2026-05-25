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
    namespace {
        /** @brief Tile offset build shader execution modes. */
        enum class TileOffsetMode : uint32_t {
            ScanTileChunks = 0,
            ScanChunkSums = 1,
            AddChunkOffsets = 2,
            FinalizeStats = 3,
        };
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
        for (uint32_t i = 0; i < rhi::kMaxFramesInFlight; ++i) {
            stats_readback_buffers_[i] = rm_->create_buffer({
                .size = sizeof(GsRuntimeStats),
                .usage = rhi::BufferUsage::TransferDst,
                .memory = rhi::MemoryUsage::GpuToCpu,
            }, "GS Runtime Stats Readback");
        }

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
        (void)indirect_dispatch_buffer;

        consume_delayed_stats(frame_ctx.frame_index);

        if (max_splat_count == 0 ||
            !projected_splat_buffer.valid() ||
            !depth_key_buffer.valid() ||
            !visible_counter_buffer.valid() ||
            count_pipeline_.pipeline == VK_NULL_HANDLE ||
            offset_pipeline_.pipeline == VK_NULL_HANDLE ||
            scatter_pipeline_.pipeline == VK_NULL_HANDLE) {
            return;
        }

        ensure_capacity(max_splat_count, screen_width, screen_height);
        if (!tile_buffers_.tile_requested_counts_buffer().valid() ||
            !tile_buffers_.tile_offsets_buffer().valid() ||
            !tile_buffers_.tile_counts_buffer().valid() ||
            !tile_buffers_.tile_cursors_buffer().valid() ||
            !tile_buffers_.entry_depth_keys_buffer().valid() ||
            !tile_buffers_.entry_splat_ids_buffer().valid() ||
            !tile_buffers_.entry_stats_buffer().valid() ||
            tile_buffers_.tile_count() == 0 ||
            tile_buffers_.scan_chunk_count() == 0 ||
            tile_buffers_.entry_capacity() == 0) {
            return;
        }

        const auto &tile_requested_counts = rm_->get_buffer(tile_buffers_.tile_requested_counts_buffer());
        const auto &tile_offsets = rm_->get_buffer(tile_buffers_.tile_offsets_buffer());
        const auto &tile_counts = rm_->get_buffer(tile_buffers_.tile_counts_buffer());
        const auto &tile_cursors = rm_->get_buffer(tile_buffers_.tile_cursors_buffer());
        const auto &entry_stats = rm_->get_buffer(tile_buffers_.entry_stats_buffer());

        vkCmdFillBuffer(cmd.handle(), tile_requested_counts.buffer, 0, tile_requested_counts.desc.size, 0u);
        vkCmdFillBuffer(cmd.handle(), tile_offsets.buffer, 0, tile_offsets.desc.size, 0u);
        vkCmdFillBuffer(cmd.handle(), tile_counts.buffer, 0, tile_counts.desc.size, 0u);
        vkCmdFillBuffer(cmd.handle(), tile_cursors.buffer, 0, tile_cursors.desc.size, 0u);
        vkCmdFillBuffer(cmd.handle(), entry_stats.buffer, 0, entry_stats.desc.size, 0u);

        rhi::buffer_barrier(cmd,
                            *rm_,
                            visible_counter_buffer,
                            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                            VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                            VK_PIPELINE_STAGE_2_TRANSFER_BIT,
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
                            VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                            VK_ACCESS_2_TRANSFER_READ_BIT,
                            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                            VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
        barrier_transfer_outputs_to_compute(cmd);

        const auto projected_info = rhi::storage_buffer_info(*rm_, projected_splat_buffer);
        const auto depth_key_info = rhi::storage_buffer_info(*rm_, depth_key_buffer);
        const auto visible_counter_info = rhi::storage_buffer_info(*rm_, visible_counter_buffer);
        const auto tile_requested_counts_info = rhi::storage_buffer_info(*rm_, tile_buffers_.tile_requested_counts_buffer());
        const auto tile_offsets_info = rhi::storage_buffer_info(*rm_, tile_buffers_.tile_offsets_buffer());
        const auto tile_counts_info = rhi::storage_buffer_info(*rm_, tile_buffers_.tile_counts_buffer());
        const auto tile_cursors_info = rhi::storage_buffer_info(*rm_, tile_buffers_.tile_cursors_buffer());
        const auto entry_depth_keys_info = rhi::storage_buffer_info(*rm_, tile_buffers_.entry_depth_keys_buffer());
        const auto entry_splat_ids_info = rhi::storage_buffer_info(*rm_, tile_buffers_.entry_splat_ids_buffer());
        const auto entry_stats_info = rhi::storage_buffer_info(*rm_, tile_buffers_.entry_stats_buffer());
        const auto scan_chunk_sums_info = rhi::storage_buffer_info(*rm_, tile_buffers_.scan_chunk_sums_buffer());
        const auto scan_chunk_offsets_info = rhi::storage_buffer_info(*rm_, tile_buffers_.scan_chunk_offsets_buffer());

        const TileCoveragePushConstants coverage_pc{
            .max_splat_count = max_splat_count,
            .tile_count_x = tile_buffers_.tile_count_x(),
            .tile_count_y = tile_buffers_.tile_count_y(),
            ._padding = 0,
        };

        cmd.bind_compute_pipeline(count_pipeline_);
        rhi::bind_dispatch_descriptor_sets(cmd, *dm_, count_pipeline_, frame_ctx.frame_index);
        {
            const std::array infos = {
                projected_info,
                visible_counter_info,
                tile_requested_counts_info,
                entry_stats_info,
            };
            rhi::push_storage_buffers(cmd, count_pipeline_, infos);
            cmd.push_constants(count_pipeline_.layout, VK_SHADER_STAGE_COMPUTE_BIT, &coverage_pc, sizeof(coverage_pc));
            const uint32_t group_count = (max_splat_count + kWorkgroupSize - 1u) / kWorkgroupSize;
            cmd.dispatch(group_count, 1, 1);
        }
        barrier_count_outputs_to_compute(cmd);

        const std::array offset_infos = {
            tile_requested_counts_info,
            tile_offsets_info,
            tile_counts_info,
            entry_stats_info,
            scan_chunk_sums_info,
            scan_chunk_offsets_info,
        };
        cmd.bind_compute_pipeline(offset_pipeline_);
        rhi::bind_dispatch_descriptor_sets(cmd, *dm_, offset_pipeline_, frame_ctx.frame_index);
        rhi::push_storage_buffers(cmd, offset_pipeline_, offset_infos);
        TileOffsetPushConstants offset_pc{
            .mode = static_cast<uint32_t>(TileOffsetMode::ScanTileChunks),
            .tile_count = tile_buffers_.tile_count(),
            .max_entries_per_tile = tile_buffers_.max_entries_per_tile(),
            .chunk_count = tile_buffers_.scan_chunk_count(),
            .entry_capacity = tile_buffers_.entry_capacity(),
            ._padding0 = 0,
            ._padding1 = 0,
            ._padding2 = 0,
        };
        cmd.push_constants(offset_pipeline_.layout, VK_SHADER_STAGE_COMPUTE_BIT, &offset_pc, sizeof(offset_pc));
        cmd.dispatch(tile_buffers_.scan_chunk_count(), 1, 1);
        barrier_offset_outputs_to_compute(cmd);

        offset_pc.mode = static_cast<uint32_t>(TileOffsetMode::ScanChunkSums);
        cmd.push_constants(offset_pipeline_.layout, VK_SHADER_STAGE_COMPUTE_BIT, &offset_pc, sizeof(offset_pc));
        cmd.dispatch(1, 1, 1);
        barrier_offset_outputs_to_compute(cmd);

        offset_pc.mode = static_cast<uint32_t>(TileOffsetMode::AddChunkOffsets);
        cmd.push_constants(offset_pipeline_.layout, VK_SHADER_STAGE_COMPUTE_BIT, &offset_pc, sizeof(offset_pc));
        cmd.dispatch(tile_buffers_.scan_chunk_count(), 1, 1);
        barrier_offset_outputs_to_compute(cmd);

        offset_pc.mode = static_cast<uint32_t>(TileOffsetMode::FinalizeStats);
        cmd.push_constants(offset_pipeline_.layout, VK_SHADER_STAGE_COMPUTE_BIT, &offset_pc, sizeof(offset_pc));
        cmd.dispatch(1, 1, 1);
        barrier_offset_outputs_to_compute(cmd);

        cmd.bind_compute_pipeline(scatter_pipeline_);
        rhi::bind_dispatch_descriptor_sets(cmd, *dm_, scatter_pipeline_, frame_ctx.frame_index);
        {
            const std::array infos = {
                projected_info,
                depth_key_info,
                visible_counter_info,
                tile_offsets_info,
                tile_counts_info,
                tile_cursors_info,
                entry_depth_keys_info,
                entry_splat_ids_info,
            };
            rhi::push_storage_buffers(cmd, scatter_pipeline_, infos);
            cmd.push_constants(scatter_pipeline_.layout, VK_SHADER_STAGE_COMPUTE_BIT, &coverage_pc, sizeof(coverage_pc));
            const uint32_t group_count = (max_splat_count + kWorkgroupSize - 1u) / kWorkgroupSize;
            cmd.dispatch(group_count, 1, 1);
        }
        barrier_scatter_outputs_to_compute_read(cmd);
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
    }

    void GsTileBinningPass::reset_scene_state() {
        tile_buffers_.destroy();
        stats_readback_valid_.fill(false);
        runtime_stats_ = {};
        has_runtime_stats_ = false;
        warned_entry_dropped_ = false;
        warned_invalid_entries_ = false;
    }

    void GsTileBinningPass::destroy() {
        tile_buffers_.destroy();
        destroy_readback_buffers();
        destroy_pipelines();

        if (count_set3_layout_ != VK_NULL_HANDLE) {
            vkDestroyDescriptorSetLayout(ctx_->device, count_set3_layout_, nullptr);
            count_set3_layout_ = VK_NULL_HANDLE;
        }
        if (offset_set3_layout_ != VK_NULL_HANDLE) {
            vkDestroyDescriptorSetLayout(ctx_->device, offset_set3_layout_, nullptr);
            offset_set3_layout_ = VK_NULL_HANDLE;
        }
        if (scatter_set3_layout_ != VK_NULL_HANDLE) {
            vkDestroyDescriptorSetLayout(ctx_->device, scatter_set3_layout_, nullptr);
            scatter_set3_layout_ = VK_NULL_HANDLE;
        }
    }

    bool GsTileBinningPass::is_ready() const {
        return count_pipeline_.pipeline != VK_NULL_HANDLE &&
               offset_pipeline_.pipeline != VK_NULL_HANDLE &&
               scatter_pipeline_.pipeline != VK_NULL_HANDLE;
    }

    const GsTileBuffers &GsTileBinningPass::tile_buffers() const {
        return tile_buffers_;
    }

    bool GsTileBinningPass::has_runtime_stats() const {
        return has_runtime_stats_;
    }

    const GsRuntimeStats &GsTileBinningPass::runtime_stats() const {
        return runtime_stats_;
    }

    void GsTileBinningPass::create_descriptor_layouts() {
        const std::array count_bindings = {
            rhi::storage_buffer_binding(0),
            rhi::storage_buffer_binding(1),
            rhi::storage_buffer_binding(2),
            rhi::storage_buffer_binding(3),
        };
        count_set3_layout_ = rhi::create_push_storage_descriptor_set_layout(*ctx_, count_bindings);

        const std::array offset_bindings = {
            rhi::storage_buffer_binding(0),
            rhi::storage_buffer_binding(1),
            rhi::storage_buffer_binding(2),
            rhi::storage_buffer_binding(3),
            rhi::storage_buffer_binding(4),
            rhi::storage_buffer_binding(5),
        };
        offset_set3_layout_ = rhi::create_push_storage_descriptor_set_layout(*ctx_, offset_bindings);

        const std::array scatter_bindings = {
            rhi::storage_buffer_binding(0),
            rhi::storage_buffer_binding(1),
            rhi::storage_buffer_binding(2),
            rhi::storage_buffer_binding(3),
            rhi::storage_buffer_binding(4),
            rhi::storage_buffer_binding(5),
            rhi::storage_buffer_binding(6),
            rhi::storage_buffer_binding(7),
        };
        scatter_set3_layout_ = rhi::create_push_storage_descriptor_set_layout(*ctx_, scatter_bindings);
    }

    void GsTileBinningPass::create_pipelines() {
        const std::array coverage_push_ranges = {
            VkPushConstantRange{
                .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
                .offset = 0,
                .size = sizeof(TileCoveragePushConstants),
            },
        };
        const std::array offset_push_ranges = {
            VkPushConstantRange{
                .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
                .offset = 0,
                .size = sizeof(TileOffsetPushConstants),
            },
        };

        auto count_pipeline = rhi::create_compute_pipeline_from_file(*ctx_, *dm_, *sc_,
                                                                     "gs/gs_tile_count.comp",
                                                                     count_set3_layout_,
                                                                     coverage_push_ranges);
        auto offset_pipeline = rhi::create_compute_pipeline_from_file(*ctx_, *dm_, *sc_,
                                                                      "gs/gs_tile_offset.comp",
                                                                      offset_set3_layout_,
                                                                      offset_push_ranges);
        auto scatter_pipeline = rhi::create_compute_pipeline_from_file(*ctx_, *dm_, *sc_,
                                                                       "gs/gs_tile_scatter.comp",
                                                                       scatter_set3_layout_,
                                                                       coverage_push_ranges);

        if (count_pipeline.pipeline == VK_NULL_HANDLE ||
            offset_pipeline.pipeline == VK_NULL_HANDLE ||
            scatter_pipeline.pipeline == VK_NULL_HANDLE) {
            if (count_pipeline.pipeline != VK_NULL_HANDLE) {
                count_pipeline.destroy(ctx_->device);
            }
            if (offset_pipeline.pipeline != VK_NULL_HANDLE) {
                offset_pipeline.destroy(ctx_->device);
            }
            if (scatter_pipeline.pipeline != VK_NULL_HANDLE) {
                scatter_pipeline.destroy(ctx_->device);
            }
            spdlog::warn("GsTileBinningPass: shader compilation failed, keeping previous pipelines");
            return;
        }

        destroy_pipelines();
        count_pipeline_ = count_pipeline;
        offset_pipeline_ = offset_pipeline;
        scatter_pipeline_ = scatter_pipeline;
    }

    void GsTileBinningPass::destroy_pipelines() {
        if (count_pipeline_.pipeline != VK_NULL_HANDLE) {
            count_pipeline_.destroy(ctx_->device);
            count_pipeline_ = {};
        }
        if (offset_pipeline_.pipeline != VK_NULL_HANDLE) {
            offset_pipeline_.destroy(ctx_->device);
            offset_pipeline_ = {};
        }
        if (scatter_pipeline_.pipeline != VK_NULL_HANDLE) {
            scatter_pipeline_.destroy(ctx_->device);
            scatter_pipeline_ = {};
        }
    }

    void GsTileBinningPass::barrier_transfer_outputs_to_compute(const rhi::CommandBuffer &cmd) const {
        const std::array buffers = {
            tile_buffers_.tile_requested_counts_buffer(),
            tile_buffers_.tile_offsets_buffer(),
            tile_buffers_.tile_counts_buffer(),
            tile_buffers_.tile_cursors_buffer(),
            tile_buffers_.entry_stats_buffer(),
        };
        rhi::buffer_barriers(cmd,
                             *rm_,
                             buffers,
                             VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                             VK_ACCESS_2_TRANSFER_WRITE_BIT,
                             VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                             VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    }

    void GsTileBinningPass::barrier_count_outputs_to_compute(const rhi::CommandBuffer &cmd) const {
        const std::array buffers = {
            tile_buffers_.tile_requested_counts_buffer(),
            tile_buffers_.entry_stats_buffer(),
        };
        rhi::buffer_barriers(cmd,
                             *rm_,
                             buffers,
                             VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                             VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                             VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                             VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    }

    void GsTileBinningPass::barrier_offset_outputs_to_compute(const rhi::CommandBuffer &cmd) const {
        const std::array buffers = {
            tile_buffers_.tile_offsets_buffer(),
            tile_buffers_.tile_counts_buffer(),
            tile_buffers_.entry_stats_buffer(),
            tile_buffers_.scan_chunk_sums_buffer(),
            tile_buffers_.scan_chunk_offsets_buffer(),
        };
        rhi::buffer_barriers(cmd,
                             *rm_,
                             buffers,
                             VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                             VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                             VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                             VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    }

    void GsTileBinningPass::barrier_scatter_outputs_to_compute_read(const rhi::CommandBuffer &cmd) const {
        const std::array buffers = {
            tile_buffers_.tile_cursors_buffer(),
            tile_buffers_.entry_depth_keys_buffer(),
            tile_buffers_.entry_splat_ids_buffer(),
            tile_buffers_.entry_stats_buffer(),
        };
        rhi::buffer_barriers(cmd,
                             *rm_,
                             buffers,
                             VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                             VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                             VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                             VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
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
                            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                            VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT,
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
            const float dropped_ratio = runtime_stats_.entry_requested == 0
                                            ? 0.0F
                                            : static_cast<float>(runtime_stats_.entry_dropped) /
                                              static_cast<float>(runtime_stats_.entry_requested);
            spdlog::warn("GS output is not correctness-valid: per-tile capacity overflow "
                         "requested={}, written={}, dropped={}, dropped_ratio={:.2f}%, max_tile_requested={}, per_tile_capacity={}",
                         runtime_stats_.entry_requested,
                         runtime_stats_.entry_written,
                         runtime_stats_.entry_dropped,
                         dropped_ratio * 100.0F,
                         runtime_stats_.max_tile_requested,
                         tile_buffers_.max_entries_per_tile());
            warned_entry_dropped_ = true;
        }

        if (runtime_stats_.invalid_entries > 0 && !warned_invalid_entries_) {
            spdlog::warn("GS invalid tile entries detected: invalid_entries={}", runtime_stats_.invalid_entries);
            warned_invalid_entries_ = true;
        }
    }
} // namespace himalaya::passes
