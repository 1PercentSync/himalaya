/**
 * @file gaussian_splat_bitonic_sort_pass.cpp
 * @brief GaussianSplatBitonicSortPass implementation.
 */

#include <himalaya/passes/gaussian_splat_bitonic_sort_pass.h>

#include <himalaya/framework/gaussian_splat_data.h>
#include <himalaya/framework/render_graph.h>
#include <himalaya/rhi/commands.h>
#include <himalaya/rhi/context.h>
#include <himalaya/rhi/descriptors.h>
#include <himalaya/rhi/resources.h>
#include <himalaya/rhi/shader.h>

#include <array>

#include <spdlog/spdlog.h>

namespace himalaya::passes {
    namespace {
        /** @brief Computes ceil(value / divisor) for positive workgroup sizing. */
        uint32_t ceil_div(const uint32_t value, const uint32_t divisor) {
            return (value + divisor - 1u) / divisor;
        }

        /** @brief Inserts a compute-to-compute SSBO barrier for the primary sort-entry buffer. */
        void barrier_sort_entries(const rhi::CommandBuffer &cmd,
                                  const rhi::ResourceManager &rm,
                                  const rhi::BufferHandle sort_entries_buffer) {
            const auto &buffer = rm.get_buffer(sort_entries_buffer);

            VkBufferMemoryBarrier2 barrier{};
            barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
            barrier.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            barrier.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
            barrier.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            barrier.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
            barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.buffer = buffer.buffer;
            barrier.offset = 0;
            barrier.size = VK_WHOLE_SIZE;

            VkDependencyInfo dependency{};
            dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
            dependency.bufferMemoryBarrierCount = 1;
            dependency.pBufferMemoryBarriers = &barrier;

            cmd.pipeline_barrier(dependency);
        }
    } // namespace

    void GaussianSplatBitonicSortPass::setup(rhi::Context &ctx,
                                             rhi::ResourceManager &rm,
                                             rhi::DescriptorManager &dm,
                                             rhi::ShaderCompiler &sc,
                                             const VkDescriptorSetLayout gs_set3_layout) {
        ctx_ = &ctx;
        rm_ = &rm;
        dm_ = &dm;
        sc_ = &sc;
        gs_set3_layout_ = gs_set3_layout;

        create_pipeline();
    }

    void GaussianSplatBitonicSortPass::record(framework::RenderGraph &rg,
                                              const GaussianSplatGraphResources &resources,
                                              const framework::GaussianSplatGpuScene &scene,
                                              const uint32_t frame_index) const {
        if (pipeline_.pipeline == VK_NULL_HANDLE || !rm_ || scene.sort_capacity <= 1u ||
            scene.descriptor_set == VK_NULL_HANDLE) {
            return;
        }

        const std::array usages = {
            framework::RGResourceUsage{
                resources.sort_entries,
                framework::RGAccessType::ReadWrite,
                framework::RGStage::Compute,
            },
        };

        rg.add_pass("GS Bitonic Sort", usages,
                    [this, &rg, resources, scene, frame_index](const rhi::CommandBuffer &cmd) {
                        cmd.bind_compute_pipeline(pipeline_);

                        const std::array sets = {
                            dm_->get_set0(frame_index),
                            dm_->get_set1(),
                            dm_->get_set2(frame_index),
                            scene.descriptor_set,
                        };
                        cmd.bind_compute_descriptor_sets(pipeline_.layout,
                                                         0,
                                                         sets.data(),
                                                         static_cast<uint32_t>(sets.size()));

                        const auto sort_entries_buffer = rg.get_buffer(resources.sort_entries);
                        const uint32_t dispatch_count = ceil_div(scene.sort_capacity,
                                                                 kGaussianSplatBitonicSortWorkgroupSize);

                        for (uint32_t stage_k = 2u; stage_k <= scene.sort_capacity; stage_k <<= 1u) {
                            for (uint32_t step_j = stage_k >> 1u; step_j > 0u; step_j >>= 1u) {
                                const GaussianSplatBitonicSortPushConstants push_constants{
                                    .sort_capacity = scene.sort_capacity,
                                    .stage_k = stage_k,
                                    .step_j = step_j,
                                    .padding = 0u,
                                };

                                cmd.push_constants(pipeline_.layout,
                                                   VK_SHADER_STAGE_COMPUTE_BIT,
                                                   &push_constants,
                                                   sizeof(push_constants));
                                cmd.dispatch(dispatch_count, 1, 1);

                                const bool final_step = stage_k == scene.sort_capacity && step_j == 1u;
                                if (!final_step) {
                                    barrier_sort_entries(cmd, *rm_, sort_entries_buffer);
                                }
                            }
                        }
                    });
    }

    void GaussianSplatBitonicSortPass::rebuild_pipelines() {
        create_pipeline();
    }

    void GaussianSplatBitonicSortPass::destroy() {
        if (ctx_ && pipeline_.pipeline != VK_NULL_HANDLE) {
            pipeline_.destroy(ctx_->device);
            pipeline_ = {};
        }

        ctx_ = nullptr;
        rm_ = nullptr;
        dm_ = nullptr;
        sc_ = nullptr;
        gs_set3_layout_ = VK_NULL_HANDLE;
    }

    void GaussianSplatBitonicSortPass::create_pipeline() {
        if (!ctx_ || !dm_ || !sc_ || gs_set3_layout_ == VK_NULL_HANDLE) {
            return;
        }

        const auto spirv = sc_->compile_from_file("gs/bitonic_sort.comp", rhi::ShaderStage::Compute);
        if (spirv.empty()) {
            spdlog::warn("GaussianSplatBitonicSortPass: shader compilation failed, keeping previous pipeline");
            return;
        }

        if (pipeline_.pipeline != VK_NULL_HANDLE) {
            pipeline_.destroy(ctx_->device);
            pipeline_ = {};
        }

        const VkShaderModule shader_module = rhi::create_shader_module(ctx_->device, spirv);
        const auto set_layouts = dm_->get_dispatch_set_layouts(gs_set3_layout_);

        const VkPushConstantRange push_range{
            .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
            .offset = 0,
            .size = sizeof(GaussianSplatBitonicSortPushConstants),
        };

        const rhi::ComputePipelineDesc desc{
            .compute_shader = shader_module,
            .descriptor_set_layouts = set_layouts,
            .push_constant_ranges = {push_range},
        };

        pipeline_ = rhi::create_compute_pipeline(ctx_->device, desc);

        vkDestroyShaderModule(ctx_->device, shader_module, nullptr);
    }
} // namespace himalaya::passes
