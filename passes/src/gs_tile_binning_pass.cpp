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
                                   const rhi::BufferHandle depth_key_buffer,
                                   const rhi::BufferHandle visible_counter_buffer,
                                   const rhi::BufferHandle indirect_dispatch_buffer,
                                   const uint32_t max_splat_count,
                                   const uint32_t screen_width,
                                   const uint32_t screen_height) {
        if (max_splat_count == 0 || entry_pipeline_.pipeline == VK_NULL_HANDLE) {
            return;
        }

        ensure_capacity(max_splat_count, screen_width, screen_height);
        if (!tile_buffers_.entry_depth_keys_buffer().valid() ||
            !tile_buffers_.entry_stats_buffer().valid() ||
            tile_buffers_.tile_count() == 0 ||
            tile_buffers_.entry_capacity() == 0) {
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

        const auto &entry_stats = rm_->get_buffer(tile_buffers_.entry_stats_buffer());
        const auto &tile_offsets = rm_->get_buffer(tile_buffers_.tile_offsets_buffer());
        const auto &tile_counts = rm_->get_buffer(tile_buffers_.tile_counts_buffer());
        const auto &indirect_dispatch = rm_->get_buffer(indirect_dispatch_buffer);

        vkCmdFillBuffer(cmd.handle(), entry_stats.buffer, 0, entry_stats.desc.size, 0u);
        vkCmdFillBuffer(cmd.handle(), tile_offsets.buffer, 0, tile_offsets.desc.size, 0u);
        vkCmdFillBuffer(cmd.handle(), tile_counts.buffer, 0, tile_counts.desc.size, 0u);
        buffer_barrier(cmd,
                       tile_buffers_.entry_stats_buffer(),
                       transfer_stage,
                       transfer_write,
                       compute_stage,
                       storage_read | storage_write);
        buffer_barrier(cmd,
                       tile_buffers_.tile_offsets_buffer(),
                       transfer_stage,
                       transfer_write,
                       compute_stage,
                       storage_read | storage_write);
        buffer_barrier(cmd,
                       tile_buffers_.tile_counts_buffer(),
                       transfer_stage,
                       transfer_write,
                       compute_stage,
                       storage_read | storage_write);

        const auto projected_info = buffer_info(*rm_, projected_splat_buffer);
        const auto depth_key_info = buffer_info(*rm_, depth_key_buffer);
        const auto visible_counter_info = buffer_info(*rm_, visible_counter_buffer);
        const auto entry_stats_info = buffer_info(*rm_, tile_buffers_.entry_stats_buffer());
        const auto entry_depth_keys_info = buffer_info(*rm_, tile_buffers_.entry_depth_keys_buffer());
        const auto entry_tile_ids_info = buffer_info(*rm_, tile_buffers_.entry_tile_ids_buffer());
        const auto entry_splat_ids_info = buffer_info(*rm_, tile_buffers_.entry_splat_ids_buffer());
        const auto entry_indices_info = buffer_info(*rm_, tile_buffers_.entry_indices_buffer());

        cmd.bind_compute_pipeline(entry_pipeline_);
        bind_global_sets(entry_pipeline_);
        {
            const std::array infos = {
                projected_info,
                depth_key_info,
                visible_counter_info,
                entry_stats_info,
                entry_depth_keys_info,
                entry_tile_ids_info,
                entry_splat_ids_info,
                entry_indices_info,
            };
            push_buffers(entry_pipeline_, infos);
            const EntryPushConstants pc{
                .max_splat_count = max_splat_count,
                .entry_capacity = tile_buffers_.entry_capacity(),
                .tile_count_x = tile_buffers_.tile_count_x(),
                .tile_count_y = tile_buffers_.tile_count_y(),
            };
            cmd.push_constants(entry_pipeline_.layout, VK_SHADER_STAGE_COMPUTE_BIT, &pc, sizeof(pc));
            vkCmdDispatchIndirect(cmd.handle(), indirect_dispatch.buffer, 0);
        }

        barrier_entry_outputs_to_compute_read(cmd);
    }

    void GsTileBinningPass::rebuild_pipelines() {
        create_pipelines();
    }

    void GsTileBinningPass::destroy() {
        tile_buffers_.destroy();
        destroy_pipelines();

        if (entry_set3_layout_ != VK_NULL_HANDLE) {
            vkDestroyDescriptorSetLayout(ctx_->device, entry_set3_layout_, nullptr);
            entry_set3_layout_ = VK_NULL_HANDLE;
        }
    }

    const GsTileBuffers &GsTileBinningPass::tile_buffers() const {
        return tile_buffers_;
    }

    void GsTileBinningPass::create_descriptor_layouts() {
        const std::array entry_bindings = {
            storage_binding(0),
            storage_binding(1),
            storage_binding(2),
            storage_binding(3),
            storage_binding(4),
            storage_binding(5),
            storage_binding(6),
            storage_binding(7),
        };
        entry_set3_layout_ = create_push_storage_layout(*ctx_, entry_bindings);
    }

    void GsTileBinningPass::create_pipelines() {
        const std::array entry_push_ranges = {
            VkPushConstantRange{
                .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
                .offset = 0,
                .size = sizeof(EntryPushConstants),
            },
        };

        auto entry_pipeline = create_tile_pipeline(*ctx_, *dm_, *sc_,
                                                   "gs/gs_tile_entry.comp",
                                                   entry_set3_layout_,
                                                   entry_push_ranges);

        if (entry_pipeline.pipeline == VK_NULL_HANDLE) {
            spdlog::warn("GsTileBinningPass: shader compilation failed, keeping previous pipeline");
            return;
        }

        destroy_pipelines();
        entry_pipeline_ = entry_pipeline;
    }

    void GsTileBinningPass::destroy_pipelines() {
        if (entry_pipeline_.pipeline != VK_NULL_HANDLE) {
            entry_pipeline_.destroy(ctx_->device);
            entry_pipeline_ = {};
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

    void GsTileBinningPass::barrier_entry_outputs_to_compute_read(const rhi::CommandBuffer &cmd) const {
        const auto compute_stage = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        const auto storage_read = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
        const auto storage_write = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;

        buffer_barrier(cmd,
                       tile_buffers_.entry_stats_buffer(),
                       compute_stage,
                       storage_write,
                       compute_stage,
                       storage_read | storage_write);
        buffer_barrier(cmd,
                       tile_buffers_.entry_depth_keys_buffer(),
                       compute_stage,
                       storage_write,
                       compute_stage,
                       storage_read);
        buffer_barrier(cmd,
                       tile_buffers_.entry_tile_ids_buffer(),
                       compute_stage,
                       storage_write,
                       compute_stage,
                       storage_read);
        buffer_barrier(cmd,
                       tile_buffers_.entry_splat_ids_buffer(),
                       compute_stage,
                       storage_write,
                       compute_stage,
                       storage_read);
        buffer_barrier(cmd,
                       tile_buffers_.entry_indices_buffer(),
                       compute_stage,
                       storage_write,
                       compute_stage,
                       storage_read);
    }
} // namespace himalaya::passes
