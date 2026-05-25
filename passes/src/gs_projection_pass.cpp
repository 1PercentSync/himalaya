#include <himalaya/passes/gs_projection_pass.h>

/**
 * @file gs_projection_pass.cpp
 * @brief GsProjectionPass implementation.
 */

#include <himalaya/framework/frame_context.h>
#include <himalaya/framework/gaussian_splat_data.h>
#include <himalaya/framework/gs_gpu_data.h>
#include <himalaya/rhi/commands.h>
#include <himalaya/rhi/compute_utils.h>
#include <himalaya/rhi/context.h>
#include <himalaya/rhi/descriptors.h>
#include <himalaya/rhi/resources.h>

#include <array>

#include <spdlog/spdlog.h>

namespace himalaya::passes {
    namespace {
        constexpr uint32_t kProjectionWorkgroupSize = 256;
    }

    void GsProjectionPass::setup(rhi::Context &ctx,
                                 rhi::ResourceManager &rm,
                                 rhi::DescriptorManager &dm,
                                 rhi::ShaderCompiler &sc) {
        ctx_ = &ctx;
        rm_ = &rm;
        dm_ = &dm;
        sc_ = &sc;

        const std::array bindings = {
            rhi::storage_buffer_binding(0),
            rhi::storage_buffer_binding(1),
            rhi::storage_buffer_binding(2),
            rhi::storage_buffer_binding(3),
            rhi::storage_buffer_binding(4),
        };
        set3_layout_ = rhi::create_push_storage_descriptor_set_layout(*ctx_, bindings);

        create_pipeline();
    }

    void GsProjectionPass::ensure_capacity(const uint32_t max_splat_count) {
        if (max_splat_count == max_splat_count_) {
            return;
        }

        destroy_buffers();
        max_splat_count_ = max_splat_count;

        if (max_splat_count_ == 0) {
            return;
        }

        const uint64_t projected_size =
            static_cast<uint64_t>(max_splat_count_) * sizeof(framework::GSSplatData2D);
        const uint64_t depth_key_size =
            static_cast<uint64_t>(max_splat_count_) * sizeof(uint32_t);

        projected_splat_buffer_ = rm_->create_buffer({
            .size = projected_size,
            .usage = rhi::BufferUsage::StorageBuffer,
            .memory = rhi::MemoryUsage::GpuOnly,
        }, "GS Projected Splats SSBO");

        depth_key_buffer_ = rm_->create_buffer({
            .size = depth_key_size,
            .usage = rhi::BufferUsage::StorageBuffer,
            .memory = rhi::MemoryUsage::GpuOnly,
        }, "GS Depth Key SSBO");

        visible_counter_buffer_ = rm_->create_buffer({
            .size = sizeof(uint32_t),
            .usage = rhi::BufferUsage::StorageBuffer | rhi::BufferUsage::TransferDst | rhi::BufferUsage::TransferSrc,
            .memory = rhi::MemoryUsage::GpuOnly,
        }, "GS Visible Counter SSBO");

        indirect_dispatch_buffer_ = rm_->create_buffer({
            .size = sizeof(VkDispatchIndirectCommand),
            .usage = rhi::BufferUsage::StorageBuffer |
                     rhi::BufferUsage::TransferDst |
                     rhi::BufferUsage::IndirectBuffer,
            .memory = rhi::MemoryUsage::GpuOnly,
        }, "GS Indirect Dispatch Buffer");

        spdlog::info("GsProjectionPass: allocated projection buffers for {} splats", max_splat_count_);
    }

    void GsProjectionPass::record(const rhi::CommandBuffer &cmd,
                                  const framework::FrameContext &frame_ctx,
                                  const framework::GsGpuData &gs_data,
                                  const uint32_t screen_width,
                                  const uint32_t screen_height) {
        if (gs_data.total_splat_count() == 0 || pipeline_.pipeline == VK_NULL_HANDLE) {
            return;
        }

        ensure_capacity(gs_data.total_splat_count());
        if (!projected_splat_buffer_.valid()) {
            return;
        }

        const auto &counter = rm_->get_buffer(visible_counter_buffer_);
        vkCmdFillBuffer(cmd.handle(), counter.buffer, 0, sizeof(uint32_t), 0u);
        rhi::buffer_barrier(cmd,
                            *rm_,
                            visible_counter_buffer_,
                            VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                            VK_ACCESS_2_TRANSFER_WRITE_BIT,
                            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                            VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                            0,
                            sizeof(uint32_t));

        cmd.bind_compute_pipeline(pipeline_);
        rhi::bind_dispatch_descriptor_sets(cmd, *dm_, pipeline_, frame_ctx.frame_index);

        const auto core_info = rhi::storage_buffer_info(*rm_, gs_data.core_buffer());
        const auto projected_info = rhi::storage_buffer_info(*rm_, projected_splat_buffer_);
        const auto counter_info = rhi::storage_buffer_info(*rm_, visible_counter_buffer_);
        const auto depth_key_info = rhi::storage_buffer_info(*rm_, depth_key_buffer_);
        for (const auto &group : gs_data.sh_groups()) {
            const auto sh_buffer_handle = gs_data.sh_buffer(group.sh_degree);
            if (!sh_buffer_handle.valid() || group.splat_count == 0) {
                continue;
            }

            const auto sh_info = rhi::storage_buffer_info(*rm_, sh_buffer_handle);
            const std::array infos = {
                core_info,
                projected_info,
                sh_info,
                counter_info,
                depth_key_info,
            };
            rhi::push_storage_buffers(cmd, pipeline_, infos);

            const PushConstants pc{
                .splat_offset = group.splat_offset,
                .splat_count = group.splat_count,
                .sh_degree = group.sh_degree,
                .screen_width = screen_width,
                .screen_height = screen_height,
            };
            cmd.push_constants(pipeline_.layout, VK_SHADER_STAGE_COMPUTE_BIT, &pc, sizeof(pc));

            const uint32_t group_count =
                (group.splat_count + kProjectionWorkgroupSize - 1u) / kProjectionWorkgroupSize;
            cmd.dispatch(group_count, 1, 1);

            // Projection dispatch groups share the visible counter and compacted
            // output buffers. Make each group visible to the next group and to
            // downstream GS stages recorded after projection.
            const std::array projection_outputs = {
                visible_counter_buffer_,
                projected_splat_buffer_,
                depth_key_buffer_,
            };
            rhi::buffer_barriers(cmd,
                                 *rm_,
                                 projection_outputs,
                                 VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                 VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                                 VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                 VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
        }
    }

    void GsProjectionPass::rebuild_pipelines() {
        create_pipeline();
    }

    void GsProjectionPass::destroy() {
        destroy_buffers();

        if (pipeline_.pipeline != VK_NULL_HANDLE) {
            pipeline_.destroy(ctx_->device);
            pipeline_ = {};
        }

        if (set3_layout_ != VK_NULL_HANDLE) {
            vkDestroyDescriptorSetLayout(ctx_->device, set3_layout_, nullptr);
            set3_layout_ = VK_NULL_HANDLE;
        }
    }

    rhi::BufferHandle GsProjectionPass::projected_splat_buffer() const {
        return projected_splat_buffer_;
    }

    rhi::BufferHandle GsProjectionPass::depth_key_buffer() const {
        return depth_key_buffer_;
    }

    rhi::BufferHandle GsProjectionPass::visible_counter_buffer() const {
        return visible_counter_buffer_;
    }

    rhi::BufferHandle GsProjectionPass::indirect_dispatch_buffer() const {
        return indirect_dispatch_buffer_;
    }

    uint32_t GsProjectionPass::max_splat_count() const {
        return max_splat_count_;
    }

    bool GsProjectionPass::is_ready() const {
        return pipeline_.pipeline != VK_NULL_HANDLE;
    }

    void GsProjectionPass::create_pipeline() {
        const std::array push_ranges = {
            VkPushConstantRange{
                .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
                .offset = 0,
                .size = sizeof(PushConstants),
            },
        };

        auto pipeline = rhi::create_compute_pipeline_from_file(*ctx_, *dm_, *sc_,
                                                               "gs/gs_projection.comp",
                                                               set3_layout_,
                                                               push_ranges);
        if (pipeline.pipeline == VK_NULL_HANDLE) {
            spdlog::warn("GsProjectionPass: shader compilation failed, keeping previous pipeline");
            return;
        }

        if (pipeline_.pipeline != VK_NULL_HANDLE) {
            pipeline_.destroy(ctx_->device);
        }
        pipeline_ = pipeline;
    }

    void GsProjectionPass::destroy_buffers() {
        if (projected_splat_buffer_.valid()) {
            rm_->destroy_buffer(projected_splat_buffer_);
            projected_splat_buffer_ = {};
        }
        if (depth_key_buffer_.valid()) {
            rm_->destroy_buffer(depth_key_buffer_);
            depth_key_buffer_ = {};
        }
        if (visible_counter_buffer_.valid()) {
            rm_->destroy_buffer(visible_counter_buffer_);
            visible_counter_buffer_ = {};
        }
        if (indirect_dispatch_buffer_.valid()) {
            rm_->destroy_buffer(indirect_dispatch_buffer_);
            indirect_dispatch_buffer_ = {};
        }

        max_splat_count_ = 0;
    }

} // namespace himalaya::passes
