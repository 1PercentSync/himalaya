#include <himalaya/framework/radix_sort.h>

/**
 * @file radix_sort.cpp
 * @brief RadixSort implementation skeleton.
 */

#include <himalaya/framework/frame_context.h>
#include <himalaya/rhi/commands.h>
#include <himalaya/rhi/compute_utils.h>
#include <himalaya/rhi/context.h>
#include <himalaya/rhi/descriptors.h>
#include <himalaya/rhi/resources.h>

#include <array>

#include <spdlog/spdlog.h>

namespace himalaya::framework {
    namespace {
        /** @brief Scan shader execution modes. */
        enum class ScanMode : uint32_t {
            ScanHistogramChunks = 0,
            ScanChunkSums = 1,
            AddChunkOffsets = 2,
            ScanDigitOffsets = 3,
        };

        /**
         * @brief Clamps sort capacity to the current scan implementation limit.
         */
        uint32_t clamp_sort_element_count(const uint32_t requested_count) {
            if (requested_count <= RadixSort::kMaxSortableElements) {
                return requested_count;
            }

            static bool warned = false;
            if (!warned) {
                spdlog::warn("RadixSort: requested {} elements exceeds current limit {}; clamping sort capacity",
                             requested_count,
                             RadixSort::kMaxSortableElements);
                warned = true;
            }

            return RadixSort::kMaxSortableElements;
        }

    } // namespace

    void RadixSort::setup(rhi::Context &ctx,
                          rhi::ResourceManager &rm,
                          rhi::DescriptorManager &dm,
                          rhi::ShaderCompiler &sc) {
        ctx_ = &ctx;
        rm_ = &rm;
        dm_ = &dm;
        sc_ = &sc;

        create_descriptor_layouts();
        create_pipelines();
    }

    void RadixSort::ensure_capacity(const uint32_t max_element_count) {
        const uint32_t clamped_max_element_count = clamp_sort_element_count(max_element_count);
        if (clamped_max_element_count == max_element_count_) {
            return;
        }

        destroy_buffers();

        if (clamped_max_element_count == 0) {
            return;
        }

        max_element_count_ = clamped_max_element_count;
        block_count_ = (max_element_count_ + kWorkgroupSize - 1u) / kWorkgroupSize;
        chunk_count_ = (block_count_ + kWorkgroupSize - 1u) / kWorkgroupSize;
        if (chunk_count_ > kMaxScanChunkCount) {
            spdlog::error("RadixSort: chunk_count {} exceeds current scan limit {}",
                          chunk_count_,
                          kMaxScanChunkCount);
            destroy_buffers();
            return;
        }

        const uint64_t element_buffer_size =
            static_cast<uint64_t>(max_element_count_) * sizeof(uint32_t);
        const uint64_t histogram_buffer_size =
            static_cast<uint64_t>(kRadixSize) * block_count_ * sizeof(uint32_t);
        const uint64_t chunk_sums_buffer_size =
            static_cast<uint64_t>(kRadixSize) * chunk_count_ * sizeof(uint32_t);
        const uint64_t digit_offsets_buffer_size =
            static_cast<uint64_t>(kRadixSize) * sizeof(uint32_t);

        const rhi::BufferDesc element_desc{
            .size = element_buffer_size,
            .usage = rhi::BufferUsage::StorageBuffer,
            .memory = rhi::MemoryUsage::GpuOnly,
        };
        key_buffers_[0] = rm_->create_buffer(element_desc, "GS Sort Key Ping SSBO");
        key_buffers_[1] = rm_->create_buffer(element_desc, "GS Sort Key Pong SSBO");
        value_buffers_[0] = rm_->create_buffer(element_desc, "GS Sort Value Ping SSBO");
        value_buffers_[1] = rm_->create_buffer(element_desc, "GS Sort Value Pong SSBO");

        const rhi::BufferDesc histogram_desc{
            .size = histogram_buffer_size,
            .usage = rhi::BufferUsage::StorageBuffer | rhi::BufferUsage::TransferDst,
            .memory = rhi::MemoryUsage::GpuOnly,
        };
        histogram_buffer_ = rm_->create_buffer(histogram_desc, "GS Sort Histogram SSBO");
        scanned_histogram_buffer_ = rm_->create_buffer(histogram_desc, "GS Sort Scanned Histogram SSBO");

        chunk_sums_buffer_ = rm_->create_buffer({
            .size = chunk_sums_buffer_size,
            .usage = rhi::BufferUsage::StorageBuffer | rhi::BufferUsage::TransferDst,
            .memory = rhi::MemoryUsage::GpuOnly,
        }, "GS Sort Chunk Sums SSBO");

        digit_offsets_buffer_ = rm_->create_buffer({
            .size = digit_offsets_buffer_size,
            .usage = rhi::BufferUsage::StorageBuffer | rhi::BufferUsage::TransferDst,
            .memory = rhi::MemoryUsage::GpuOnly,
        }, "GS Sort Digit Offsets SSBO");

        output_buffer_index_ = 0;

        spdlog::info("RadixSort: allocated buffers for {} elements ({} blocks, {} chunks)",
                     max_element_count_, block_count_, chunk_count_);
    }

    void RadixSort::record(const rhi::CommandBuffer &cmd,
                           const FrameContext &frame_ctx,
                           const rhi::BufferHandle input_key_buffer,
                           const rhi::BufferHandle input_value_buffer,
                           const rhi::BufferHandle visible_counter_buffer,
                           const rhi::BufferHandle indirect_dispatch_buffer,
                           const uint32_t max_element_count) {
        if (max_element_count == 0 ||
            prepare_pipeline_.pipeline == VK_NULL_HANDLE ||
            histogram_pipeline_.pipeline == VK_NULL_HANDLE ||
            scan_pipeline_.pipeline == VK_NULL_HANDLE ||
            scatter_pipeline_.pipeline == VK_NULL_HANDLE) {
            return;
        }

        const uint32_t effective_max_element_count = clamp_sort_element_count(max_element_count);
        if (effective_max_element_count == 0) {
            return;
        }

        ensure_capacity(effective_max_element_count);
        if (!key_buffers_[0].valid() || !value_buffers_[0].valid()) {
            return;
        }

        const auto &indirect_dispatch = rm_->get_buffer(indirect_dispatch_buffer);

        const auto storage_read = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
        const auto storage_write = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        const auto transfer_write = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        const auto indirect_read = VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT;
        const auto compute_stage = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        const auto transfer_stage = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        const auto indirect_stage = VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT;

        const auto visible_counter_info = rhi::storage_buffer_info(*rm_, visible_counter_buffer);
        const auto indirect_dispatch_info = rhi::storage_buffer_info(*rm_, indirect_dispatch_buffer);
        const auto histogram_info = rhi::storage_buffer_info(*rm_, histogram_buffer_);
        const auto scanned_histogram_info = rhi::storage_buffer_info(*rm_, scanned_histogram_buffer_);
        const auto chunk_sums_info = rhi::storage_buffer_info(*rm_, chunk_sums_buffer_);
        const auto digit_offsets_info = rhi::storage_buffer_info(*rm_, digit_offsets_buffer_);

        cmd.bind_compute_pipeline(prepare_pipeline_);
        rhi::bind_dispatch_descriptor_sets(cmd, *dm_, prepare_pipeline_, frame_ctx.frame_index);
        {
            const std::array infos = {visible_counter_info, indirect_dispatch_info};
            rhi::push_storage_buffers(cmd, prepare_pipeline_, infos);
            const PreparePushConstants pc{
                .workgroup_size = kWorkgroupSize,
                .max_element_count = max_element_count_,
            };
            cmd.push_constants(prepare_pipeline_.layout, VK_SHADER_STAGE_COMPUTE_BIT, &pc, sizeof(pc));
            cmd.dispatch(1, 1, 1);
        }
        rhi::buffer_barrier(cmd,
                            *rm_,
                            indirect_dispatch_buffer,
                            compute_stage,
                            storage_write,
                            indirect_stage,
                            indirect_read);

        for (uint32_t pass_index = 0; pass_index < kPassCount; ++pass_index) {
            const rhi::BufferHandle src_key_buffer = (pass_index == 0) ? input_key_buffer : key_buffers_[(pass_index - 1u) & 1u];
            const rhi::BufferHandle src_value_buffer = (pass_index == 0) ? input_value_buffer : value_buffers_[(pass_index - 1u) & 1u];
            const uint32_t dst_index = pass_index & 1u;
            const rhi::BufferHandle dst_key_buffer = key_buffers_[dst_index];
            const rhi::BufferHandle dst_value_buffer = value_buffers_[dst_index];

            const auto src_key_info = rhi::storage_buffer_info(*rm_, src_key_buffer);
            const auto src_value_info = rhi::storage_buffer_info(*rm_, src_value_buffer);
            const auto dst_key_info = rhi::storage_buffer_info(*rm_, dst_key_buffer);
            const auto dst_value_info = rhi::storage_buffer_info(*rm_, dst_value_buffer);

            const auto &histogram_buffer = rm_->get_buffer(histogram_buffer_);
            const auto &scanned_histogram_buffer = rm_->get_buffer(scanned_histogram_buffer_);
            const auto &chunk_sums_buffer = rm_->get_buffer(chunk_sums_buffer_);
            const auto &digit_offsets_buffer = rm_->get_buffer(digit_offsets_buffer_);
            vkCmdFillBuffer(cmd.handle(), histogram_buffer.buffer, 0, histogram_buffer.desc.size, 0u);
            vkCmdFillBuffer(cmd.handle(), scanned_histogram_buffer.buffer, 0, scanned_histogram_buffer.desc.size, 0u);
            vkCmdFillBuffer(cmd.handle(), chunk_sums_buffer.buffer, 0, chunk_sums_buffer.desc.size, 0u);
            vkCmdFillBuffer(cmd.handle(), digit_offsets_buffer.buffer, 0, digit_offsets_buffer.desc.size, 0u);
            barrier_sort_buffers(cmd,
                                 transfer_stage,
                                 transfer_write,
                                 compute_stage,
                                 storage_read | storage_write);

            cmd.bind_compute_pipeline(histogram_pipeline_);
            rhi::bind_dispatch_descriptor_sets(cmd, *dm_, histogram_pipeline_, frame_ctx.frame_index);
            {
                const std::array infos = {src_key_info, histogram_info, visible_counter_info};
                rhi::push_storage_buffers(cmd, histogram_pipeline_, infos);
                const HistogramPushConstants pc{
                    .element_count = max_element_count_,
                    .pass_index = pass_index,
                    .block_count = block_count_,
                };
                cmd.push_constants(histogram_pipeline_.layout, VK_SHADER_STAGE_COMPUTE_BIT, &pc, sizeof(pc));
                vkCmdDispatchIndirect(cmd.handle(), indirect_dispatch.buffer, 0);
            }
            rhi::buffer_barrier(cmd,
                                *rm_,
                                histogram_buffer_,
                                compute_stage,
                                storage_write,
                                compute_stage,
                                storage_read);

            cmd.bind_compute_pipeline(scan_pipeline_);
            rhi::bind_dispatch_descriptor_sets(cmd, *dm_, scan_pipeline_, frame_ctx.frame_index);
            {
                const std::array infos = {
                    histogram_info,
                    scanned_histogram_info,
                    chunk_sums_info,
                    digit_offsets_info,
                };
                rhi::push_storage_buffers(cmd, scan_pipeline_, infos);

                ScanPushConstants pc{
                    .mode = static_cast<uint32_t>(ScanMode::ScanHistogramChunks),
                    .block_count = block_count_,
                    .chunk_count = chunk_count_,
                    ._padding = 0,
                };
                cmd.push_constants(scan_pipeline_.layout, VK_SHADER_STAGE_COMPUTE_BIT, &pc, sizeof(pc));
                cmd.dispatch(kRadixSize, chunk_count_, 1);
                rhi::buffer_barrier(cmd,
                                    *rm_,
                                    chunk_sums_buffer_,
                                    compute_stage,
                                    storage_write,
                                    compute_stage,
                                    storage_read | storage_write);

                pc.mode = static_cast<uint32_t>(ScanMode::ScanChunkSums);
                cmd.push_constants(scan_pipeline_.layout, VK_SHADER_STAGE_COMPUTE_BIT, &pc, sizeof(pc));
                cmd.dispatch(kRadixSize, 1, 1);
                barrier_sort_buffers(cmd,
                                     compute_stage,
                                     storage_write,
                                     compute_stage,
                                     storage_read | storage_write);

                pc.mode = static_cast<uint32_t>(ScanMode::AddChunkOffsets);
                cmd.push_constants(scan_pipeline_.layout, VK_SHADER_STAGE_COMPUTE_BIT, &pc, sizeof(pc));
                cmd.dispatch(kRadixSize, chunk_count_, 1);
                rhi::buffer_barrier(cmd,
                                    *rm_,
                                    digit_offsets_buffer_,
                                    compute_stage,
                                    storage_write,
                                    compute_stage,
                                    storage_read | storage_write);

                pc.mode = static_cast<uint32_t>(ScanMode::ScanDigitOffsets);
                cmd.push_constants(scan_pipeline_.layout, VK_SHADER_STAGE_COMPUTE_BIT, &pc, sizeof(pc));
                cmd.dispatch(1, 1, 1);
            }
            barrier_sort_buffers(cmd,
                                 compute_stage,
                                 storage_write,
                                 compute_stage,
                                 storage_read | storage_write);

            cmd.bind_compute_pipeline(scatter_pipeline_);
            rhi::bind_dispatch_descriptor_sets(cmd, *dm_, scatter_pipeline_, frame_ctx.frame_index);
            {
                const std::array infos = {
                    src_key_info,
                    src_value_info,
                    dst_key_info,
                    dst_value_info,
                    scanned_histogram_info,
                    digit_offsets_info,
                    visible_counter_info,
                };
                rhi::push_storage_buffers(cmd, scatter_pipeline_, infos);
                const ScatterPushConstants pc{
                    .element_count = max_element_count_,
                    .pass_index = pass_index,
                    .block_count = block_count_,
                };
                cmd.push_constants(scatter_pipeline_.layout, VK_SHADER_STAGE_COMPUTE_BIT, &pc, sizeof(pc));
                vkCmdDispatchIndirect(cmd.handle(), indirect_dispatch.buffer, 0);
            }
            rhi::buffer_barrier(cmd,
                                *rm_,
                                dst_key_buffer,
                                compute_stage,
                                storage_write,
                                compute_stage,
                                storage_read);
            rhi::buffer_barrier(cmd,
                                *rm_,
                                dst_value_buffer,
                                compute_stage,
                                storage_write,
                                compute_stage,
                                storage_read);

            output_buffer_index_ = dst_index;
        }
    }

    void RadixSort::rebuild_pipelines() {
        create_pipelines();
    }

    void RadixSort::destroy() {
        destroy_buffers();
        destroy_pipelines();

        if (prepare_set3_layout_ != VK_NULL_HANDLE) {
            vkDestroyDescriptorSetLayout(ctx_->device, prepare_set3_layout_, nullptr);
            prepare_set3_layout_ = VK_NULL_HANDLE;
        }
        if (histogram_set3_layout_ != VK_NULL_HANDLE) {
            vkDestroyDescriptorSetLayout(ctx_->device, histogram_set3_layout_, nullptr);
            histogram_set3_layout_ = VK_NULL_HANDLE;
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

    rhi::BufferHandle RadixSort::sorted_key_buffer() const {
        return key_buffers_[output_buffer_index_];
    }

    rhi::BufferHandle RadixSort::sorted_value_buffer() const {
        return value_buffers_[output_buffer_index_];
    }

    uint32_t RadixSort::max_element_count() const {
        return max_element_count_;
    }

    bool RadixSort::is_ready() const {
        return prepare_pipeline_.pipeline != VK_NULL_HANDLE &&
               histogram_pipeline_.pipeline != VK_NULL_HANDLE &&
               scan_pipeline_.pipeline != VK_NULL_HANDLE &&
               scatter_pipeline_.pipeline != VK_NULL_HANDLE;
    }

    void RadixSort::create_descriptor_layouts() {
        const std::array prepare_bindings = {
            rhi::storage_buffer_binding(0),
            rhi::storage_buffer_binding(1),
        };
        prepare_set3_layout_ = rhi::create_push_storage_descriptor_set_layout(*ctx_, prepare_bindings);

        const std::array histogram_bindings = {
            rhi::storage_buffer_binding(0),
            rhi::storage_buffer_binding(1),
            rhi::storage_buffer_binding(2),
        };
        histogram_set3_layout_ = rhi::create_push_storage_descriptor_set_layout(*ctx_, histogram_bindings);

        const std::array scan_bindings = {
            rhi::storage_buffer_binding(0),
            rhi::storage_buffer_binding(1),
            rhi::storage_buffer_binding(2),
            rhi::storage_buffer_binding(3),
        };
        scan_set3_layout_ = rhi::create_push_storage_descriptor_set_layout(*ctx_, scan_bindings);

        const std::array scatter_bindings = {
            rhi::storage_buffer_binding(0),
            rhi::storage_buffer_binding(1),
            rhi::storage_buffer_binding(2),
            rhi::storage_buffer_binding(3),
            rhi::storage_buffer_binding(4),
            rhi::storage_buffer_binding(5),
            rhi::storage_buffer_binding(6),
        };
        scatter_set3_layout_ = rhi::create_push_storage_descriptor_set_layout(*ctx_, scatter_bindings);
    }

    void RadixSort::create_pipelines() {
        const std::array prepare_push_ranges = {
            VkPushConstantRange{
                .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
                .offset = 0,
                .size = sizeof(PreparePushConstants),
            },
        };
        const std::array histogram_push_ranges = {
            VkPushConstantRange{
                .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
                .offset = 0,
                .size = sizeof(HistogramPushConstants),
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

        auto prepare_pipeline = rhi::create_compute_pipeline_from_file(*ctx_, *dm_, *sc_,
                                                                       "gs/gs_sort_prepare.comp",
                                                                       prepare_set3_layout_,
                                                                       prepare_push_ranges);
        auto histogram_pipeline = rhi::create_compute_pipeline_from_file(*ctx_, *dm_, *sc_,
                                                                         "gs/gs_sort_histogram.comp",
                                                                         histogram_set3_layout_,
                                                                         histogram_push_ranges);
        auto scan_pipeline = rhi::create_compute_pipeline_from_file(*ctx_, *dm_, *sc_,
                                                                    "gs/gs_sort_scan.comp",
                                                                    scan_set3_layout_,
                                                                    scan_push_ranges);
        auto scatter_pipeline = rhi::create_compute_pipeline_from_file(*ctx_, *dm_, *sc_,
                                                                       "gs/gs_sort_scatter.comp",
                                                                       scatter_set3_layout_,
                                                                       scatter_push_ranges);

        if (prepare_pipeline.pipeline == VK_NULL_HANDLE ||
            histogram_pipeline.pipeline == VK_NULL_HANDLE ||
            scan_pipeline.pipeline == VK_NULL_HANDLE ||
            scatter_pipeline.pipeline == VK_NULL_HANDLE) {
            if (prepare_pipeline.pipeline != VK_NULL_HANDLE) {
                prepare_pipeline.destroy(ctx_->device);
            }
            if (histogram_pipeline.pipeline != VK_NULL_HANDLE) {
                histogram_pipeline.destroy(ctx_->device);
            }
            if (scan_pipeline.pipeline != VK_NULL_HANDLE) {
                scan_pipeline.destroy(ctx_->device);
            }
            if (scatter_pipeline.pipeline != VK_NULL_HANDLE) {
                scatter_pipeline.destroy(ctx_->device);
            }
            spdlog::warn("RadixSort: shader compilation failed, keeping previous pipelines");
            return;
        }

        destroy_pipelines();
        prepare_pipeline_ = prepare_pipeline;
        histogram_pipeline_ = histogram_pipeline;
        scan_pipeline_ = scan_pipeline;
        scatter_pipeline_ = scatter_pipeline;
    }

    void RadixSort::destroy_pipelines() {
        if (prepare_pipeline_.pipeline != VK_NULL_HANDLE) {
            prepare_pipeline_.destroy(ctx_->device);
            prepare_pipeline_ = {};
        }
        if (histogram_pipeline_.pipeline != VK_NULL_HANDLE) {
            histogram_pipeline_.destroy(ctx_->device);
            histogram_pipeline_ = {};
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

    void RadixSort::barrier_sort_buffers(const rhi::CommandBuffer &cmd,
                                         const VkPipelineStageFlags2 src_stage,
                                         const VkAccessFlags2 src_access,
                                         const VkPipelineStageFlags2 dst_stage,
                                         const VkAccessFlags2 dst_access) const {
        const std::array buffers = {
            histogram_buffer_,
            scanned_histogram_buffer_,
            chunk_sums_buffer_,
            digit_offsets_buffer_,
        };
        rhi::buffer_barriers(cmd, *rm_, buffers, src_stage, src_access, dst_stage, dst_access);
    }

    void RadixSort::destroy_buffers() {
        for (auto &buffer : key_buffers_) {
            if (buffer.valid()) {
                rm_->destroy_buffer(buffer);
                buffer = {};
            }
        }
        for (auto &buffer : value_buffers_) {
            if (buffer.valid()) {
                rm_->destroy_buffer(buffer);
                buffer = {};
            }
        }

        if (histogram_buffer_.valid()) {
            rm_->destroy_buffer(histogram_buffer_);
            histogram_buffer_ = {};
        }
        if (scanned_histogram_buffer_.valid()) {
            rm_->destroy_buffer(scanned_histogram_buffer_);
            scanned_histogram_buffer_ = {};
        }
        if (chunk_sums_buffer_.valid()) {
            rm_->destroy_buffer(chunk_sums_buffer_);
            chunk_sums_buffer_ = {};
        }
        if (digit_offsets_buffer_.valid()) {
            rm_->destroy_buffer(digit_offsets_buffer_);
            digit_offsets_buffer_ = {};
        }

        output_buffer_index_ = 0;
        block_count_ = 0;
        chunk_count_ = 0;
        max_element_count_ = 0;
    }
} // namespace himalaya::framework
