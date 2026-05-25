/**
 * @file gs_tile_render_pass.cpp
 * @brief GsTileRenderPass implementation.
 */

#include <himalaya/passes/gs_tile_render_pass.h>

#include <himalaya/framework/frame_context.h>
#include <himalaya/passes/gs_tile_buffers.h>
#include <himalaya/rhi/commands.h>
#include <himalaya/rhi/compute_utils.h>
#include <himalaya/rhi/context.h>
#include <himalaya/rhi/descriptors.h>
#include <himalaya/rhi/resources.h>

#include <array>

#include <spdlog/spdlog.h>

namespace himalaya::passes {
    void GsTileRenderPass::setup(rhi::Context &ctx,
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
            VkDescriptorSetLayoutBinding{
                .binding = 5,
                .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                .descriptorCount = 1,
                .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
            },
        };
        set3_layout_ = rhi::create_push_storage_descriptor_set_layout(*ctx_, bindings);

        create_pipeline();
    }

    void GsTileRenderPass::record(const rhi::CommandBuffer &cmd,
                                  const framework::FrameContext &frame_ctx,
                                  const rhi::ImageHandle gs_color_image,
                                  const rhi::BufferHandle projected_splat_buffer,
                                  const GsTileBuffers &tile_buffers,
                                  const rhi::BufferHandle sorted_entry_indices_buffer) const {
        if (pipeline_.pipeline == VK_NULL_HANDLE ||
            !gs_color_image.valid() ||
            !projected_splat_buffer.valid() ||
            !sorted_entry_indices_buffer.valid() ||
            !tile_buffers.entry_splat_ids_buffer().valid() ||
            !tile_buffers.tile_offsets_buffer().valid() ||
            !tile_buffers.tile_counts_buffer().valid() ||
            tile_buffers.tile_count_x() == 0 ||
            tile_buffers.tile_count_y() == 0) {
            return;
        }

        const auto projected_info = rhi::storage_buffer_info(*rm_, projected_splat_buffer);
        const auto entry_splat_ids_info = rhi::storage_buffer_info(
            *rm_, tile_buffers.entry_splat_ids_buffer());
        const auto sorted_entry_indices_info = rhi::storage_buffer_info(
            *rm_, sorted_entry_indices_buffer);
        const auto tile_offsets_info = rhi::storage_buffer_info(
            *rm_, tile_buffers.tile_offsets_buffer());
        const auto tile_counts_info = rhi::storage_buffer_info(
            *rm_, tile_buffers.tile_counts_buffer());

        cmd.bind_compute_pipeline(pipeline_);
        rhi::bind_dispatch_descriptor_sets(cmd, *dm_, pipeline_, frame_ctx.frame_index);

        const std::array infos = {
            projected_info,
            entry_splat_ids_info,
            sorted_entry_indices_info,
            tile_offsets_info,
            tile_counts_info,
        };
        rhi::push_storage_buffers(cmd, pipeline_, infos);
        cmd.push_storage_image(*rm_, pipeline_.layout, 3, 5, gs_color_image);

        const PushConstants pc{
            .tile_count_x = tile_buffers.tile_count_x(),
            .tile_count_y = tile_buffers.tile_count_y(),
        };
        cmd.push_constants(pipeline_.layout, VK_SHADER_STAGE_COMPUTE_BIT, &pc, sizeof(pc));
        cmd.dispatch(tile_buffers.tile_count_x(), tile_buffers.tile_count_y(), 1);
    }

    void GsTileRenderPass::rebuild_pipelines() {
        create_pipeline();
    }

    void GsTileRenderPass::destroy() {
        if (pipeline_.pipeline != VK_NULL_HANDLE) {
            pipeline_.destroy(ctx_->device);
            pipeline_ = {};
        }

        if (set3_layout_ != VK_NULL_HANDLE) {
            vkDestroyDescriptorSetLayout(ctx_->device, set3_layout_, nullptr);
            set3_layout_ = VK_NULL_HANDLE;
        }
    }

    bool GsTileRenderPass::is_ready() const {
        return pipeline_.pipeline != VK_NULL_HANDLE;
    }

    void GsTileRenderPass::create_pipeline() {
        const std::array push_ranges = {
            VkPushConstantRange{
                .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
                .offset = 0,
                .size = sizeof(PushConstants),
            },
        };

        auto pipeline = rhi::create_compute_pipeline_from_file(*ctx_, *dm_, *sc_,
                                                               "gs/gs_tile_render.comp",
                                                               set3_layout_,
                                                               push_ranges);
        if (pipeline.pipeline == VK_NULL_HANDLE) {
            spdlog::warn("GsTileRenderPass: shader compilation failed, keeping previous pipeline");
            return;
        }

        if (pipeline_.pipeline != VK_NULL_HANDLE) {
            pipeline_.destroy(ctx_->device);
        }
        pipeline_ = pipeline;
    }
} // namespace himalaya::passes
