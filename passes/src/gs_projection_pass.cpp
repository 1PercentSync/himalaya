#include <himalaya/passes/gs_projection_pass.h>

/**
 * @file gs_projection_pass.cpp
 * @brief GsProjectionPass implementation.
 */

#include <himalaya/framework/frame_context.h>
#include <himalaya/framework/gaussian_splat_data.h>
#include <himalaya/framework/gs_gpu_data.h>
#include <himalaya/rhi/commands.h>
#include <himalaya/rhi/context.h>
#include <himalaya/rhi/descriptors.h>
#include <himalaya/rhi/resources.h>
#include <himalaya/rhi/shader.h>

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
            VkDescriptorSetLayoutBinding{
                .binding = 0,
                .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                .descriptorCount = 1,
                .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
            },
            VkDescriptorSetLayoutBinding{
                .binding = 1,
                .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                .descriptorCount = 1,
                .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
            },
            VkDescriptorSetLayoutBinding{
                .binding = 2,
                .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                .descriptorCount = 1,
                .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
            },
            VkDescriptorSetLayoutBinding{
                .binding = 3,
                .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                .descriptorCount = 1,
                .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
            },
            VkDescriptorSetLayoutBinding{
                .binding = 4,
                .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                .descriptorCount = 1,
                .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
            },
            VkDescriptorSetLayoutBinding{
                .binding = 5,
                .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                .descriptorCount = 1,
                .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
            },
        };

        VkDescriptorSetLayoutCreateInfo layout_ci{};
        layout_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        layout_ci.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT;
        layout_ci.bindingCount = static_cast<uint32_t>(bindings.size());
        layout_ci.pBindings = bindings.data();

        VK_CHECK(vkCreateDescriptorSetLayout(ctx_->device, &layout_ci, nullptr, &set3_layout_));

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
        const uint64_t key_value_size =
            static_cast<uint64_t>(max_splat_count_) * sizeof(uint32_t);

        projected_splat_buffer_ = rm_->create_buffer({
            .size = projected_size,
            .usage = rhi::BufferUsage::StorageBuffer,
            .memory = rhi::MemoryUsage::GpuOnly,
        }, "GS Projected Splats SSBO");

        depth_key_buffer_ = rm_->create_buffer({
            .size = key_value_size,
            .usage = rhi::BufferUsage::StorageBuffer,
            .memory = rhi::MemoryUsage::GpuOnly,
        }, "GS Depth Key SSBO");

        splat_index_buffer_ = rm_->create_buffer({
            .size = key_value_size,
            .usage = rhi::BufferUsage::StorageBuffer,
            .memory = rhi::MemoryUsage::GpuOnly,
        }, "GS Splat Index SSBO");

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
        barrier_counter_clear_to_compute(cmd);

        cmd.bind_compute_pipeline(pipeline_);

        const std::array sets = {
            dm_->get_set0(frame_ctx.frame_index),
            dm_->get_set1(),
            dm_->get_set2(frame_ctx.frame_index),
        };
        cmd.bind_compute_descriptor_sets(pipeline_.layout, 0, sets.data(), static_cast<uint32_t>(sets.size()));

        const auto &core_buffer = rm_->get_buffer(gs_data.core_buffer());
        const auto &projected_buffer = rm_->get_buffer(projected_splat_buffer_);
        const auto &depth_key_buffer = rm_->get_buffer(depth_key_buffer_);
        const auto &splat_index_buffer = rm_->get_buffer(splat_index_buffer_);

        VkDescriptorBufferInfo core_info{
            .buffer = core_buffer.buffer,
            .offset = 0,
            .range = core_buffer.desc.size,
        };
        VkDescriptorBufferInfo projected_info{
            .buffer = projected_buffer.buffer,
            .offset = 0,
            .range = projected_buffer.desc.size,
        };
        VkDescriptorBufferInfo counter_info{
            .buffer = counter.buffer,
            .offset = 0,
            .range = counter.desc.size,
        };
        VkDescriptorBufferInfo depth_key_info{
            .buffer = depth_key_buffer.buffer,
            .offset = 0,
            .range = depth_key_buffer.desc.size,
        };
        VkDescriptorBufferInfo splat_index_info{
            .buffer = splat_index_buffer.buffer,
            .offset = 0,
            .range = splat_index_buffer.desc.size,
        };

        for (const auto &group : gs_data.sh_groups()) {
            const auto sh_buffer_handle = gs_data.sh_buffer(group.sh_degree);
            if (!sh_buffer_handle.valid() || group.splat_count == 0) {
                continue;
            }

            const auto &sh_buffer = rm_->get_buffer(sh_buffer_handle);
            VkDescriptorBufferInfo sh_info{
                .buffer = sh_buffer.buffer,
                .offset = 0,
                .range = sh_buffer.desc.size,
            };

            const std::array<VkWriteDescriptorSet, 6> writes = {{
                {
                    .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                    .dstBinding = 0,
                    .descriptorCount = 1,
                    .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                    .pBufferInfo = &core_info,
                },
                {
                    .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                    .dstBinding = 1,
                    .descriptorCount = 1,
                    .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                    .pBufferInfo = &projected_info,
                },
                {
                    .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                    .dstBinding = 2,
                    .descriptorCount = 1,
                    .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                    .pBufferInfo = &sh_info,
                },
                {
                    .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                    .dstBinding = 3,
                    .descriptorCount = 1,
                    .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                    .pBufferInfo = &counter_info,
                },
                {
                    .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                    .dstBinding = 4,
                    .descriptorCount = 1,
                    .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                    .pBufferInfo = &depth_key_info,
                },
                {
                    .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                    .dstBinding = 5,
                    .descriptorCount = 1,
                    .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                    .pBufferInfo = &splat_index_info,
                },
            }};
            cmd.push_compute_descriptor_set(pipeline_.layout, 3, writes);

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
            barrier_projection_outputs_to_compute(cmd);
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

    rhi::BufferHandle GsProjectionPass::splat_index_buffer() const {
        return splat_index_buffer_;
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

    void GsProjectionPass::create_pipeline() {
        const auto spirv = sc_->compile_from_file("gs/gs_projection.comp", rhi::ShaderStage::Compute);
        if (spirv.empty()) {
            spdlog::warn("GsProjectionPass: shader compilation failed, keeping previous pipeline");
            return;
        }

        if (pipeline_.pipeline != VK_NULL_HANDLE) {
            pipeline_.destroy(ctx_->device);
        }

        const auto shader_module = rhi::create_shader_module(ctx_->device, spirv);
        const auto set_layouts = dm_->get_dispatch_set_layouts(set3_layout_);

        const VkPushConstantRange push_range{
            .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
            .offset = 0,
            .size = sizeof(PushConstants),
        };

        const rhi::ComputePipelineDesc desc{
            .compute_shader = shader_module,
            .descriptor_set_layouts = set_layouts,
            .push_constant_ranges = {push_range},
        };
        pipeline_ = rhi::create_compute_pipeline(ctx_->device, desc);

        vkDestroyShaderModule(ctx_->device, shader_module, nullptr);
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
        if (splat_index_buffer_.valid()) {
            rm_->destroy_buffer(splat_index_buffer_);
            splat_index_buffer_ = {};
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

    void GsProjectionPass::barrier_counter_clear_to_compute(const rhi::CommandBuffer &cmd) const {
        const auto &counter = rm_->get_buffer(visible_counter_buffer_);

        VkBufferMemoryBarrier2 counter_barrier{};
        counter_barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
        counter_barrier.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        counter_barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        counter_barrier.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        counter_barrier.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        counter_barrier.buffer = counter.buffer;
        counter_barrier.offset = 0;
        counter_barrier.size = sizeof(uint32_t);

        VkDependencyInfo dep{};
        dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dep.bufferMemoryBarrierCount = 1;
        dep.pBufferMemoryBarriers = &counter_barrier;
        cmd.pipeline_barrier(dep);
    }

    void GsProjectionPass::barrier_projection_outputs_to_compute(const rhi::CommandBuffer &cmd) const {
        const auto make_barrier = [this](const rhi::BufferHandle handle) {
            const auto &buffer = rm_->get_buffer(handle);

            VkBufferMemoryBarrier2 barrier{};
            barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
            barrier.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            barrier.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
            barrier.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            barrier.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
            barrier.buffer = buffer.buffer;
            barrier.offset = 0;
            barrier.size = buffer.desc.size;

            return barrier;
        };

        const std::array barriers = {
            make_barrier(visible_counter_buffer_),
            make_barrier(projected_splat_buffer_),
            make_barrier(depth_key_buffer_),
            make_barrier(splat_index_buffer_),
        };

        VkDependencyInfo dep{};
        dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dep.bufferMemoryBarrierCount = static_cast<uint32_t>(barriers.size());
        dep.pBufferMemoryBarriers = barriers.data();
        cmd.pipeline_barrier(dep);
    }
} // namespace himalaya::passes
