/**
 * @file gaussian_splat_cull_project_pass.cpp
 * @brief GaussianSplatCullProjectPass implementation.
 */

#include <himalaya/passes/gaussian_splat_cull_project_pass.h>

#include <himalaya/framework/gaussian_splat_data.h>
#include <himalaya/framework/render_graph.h>
#include <himalaya/rhi/commands.h>
#include <himalaya/rhi/context.h>
#include <himalaya/rhi/descriptors.h>
#include <himalaya/rhi/shader.h>

#include <array>

#include <spdlog/spdlog.h>

namespace himalaya::passes {
    namespace {
        /** @brief Computes ceil(value / divisor) for positive workgroup sizing. */
        uint32_t ceil_div(const uint32_t value, const uint32_t divisor) {
            return (value + divisor - 1u) / divisor;
        }
    } // namespace

    void GaussianSplatCullProjectPass::setup(rhi::Context &ctx,
                                             rhi::DescriptorManager &dm,
                                             rhi::ShaderCompiler &sc,
                                             const VkDescriptorSetLayout gs_set3_layout) {
        ctx_ = &ctx;
        dm_ = &dm;
        sc_ = &sc;
        gs_set3_layout_ = gs_set3_layout;

        create_pipeline();
    }

    void GaussianSplatCullProjectPass::record(framework::RenderGraph &rg,
                                              const GaussianSplatGraphResources &resources,
                                              const framework::GaussianSplatGpuScene &scene,
                                              const uint32_t frame_index,
                                              const GSPushConstants &push_constants) const {
        if (pipeline_.pipeline == VK_NULL_HANDLE || scene.total_splat_count == 0u ||
            scene.descriptor_set == VK_NULL_HANDLE) {
            return;
        }

        const std::array usages = {
            framework::RGResourceUsage{
                resources.position_radius,
                framework::RGAccessType::Read,
                framework::RGStage::Compute,
            },
            framework::RGResourceUsage{
                resources.covariance_opacity,
                framework::RGAccessType::Read,
                framework::RGStage::Compute,
            },
            framework::RGResourceUsage{
                resources.sh_coefficients,
                framework::RGAccessType::Read,
                framework::RGStage::Compute,
            },
            framework::RGResourceUsage{
                resources.visible_count,
                framework::RGAccessType::ReadWrite,
                framework::RGStage::Compute,
            },
            framework::RGResourceUsage{
                resources.projected_data,
                framework::RGAccessType::Write,
                framework::RGStage::Compute,
            },
            framework::RGResourceUsage{
                resources.sort_entries,
                framework::RGAccessType::Write,
                framework::RGStage::Compute,
            },
            framework::RGResourceUsage{
                resources.indirect_draw,
                framework::RGAccessType::Write,
                framework::RGStage::Compute,
            },
        };

        rg.add_pass("GS Cull Project", usages,
                    [this, scene, frame_index, push_constants](const rhi::CommandBuffer &cmd) {
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

                        cmd.push_constants(pipeline_.layout,
                                           VK_SHADER_STAGE_COMPUTE_BIT,
                                           &push_constants,
                                           sizeof(push_constants));

                        cmd.dispatch(ceil_div(push_constants.total_splat_count,
                                              kGaussianSplatCullProjectWorkgroupSize),
                                     1,
                                     1);
                    });
    }

    void GaussianSplatCullProjectPass::rebuild_pipelines() {
        create_pipeline();
    }

    void GaussianSplatCullProjectPass::destroy() {
        if (ctx_ && pipeline_.pipeline != VK_NULL_HANDLE) {
            pipeline_.destroy(ctx_->device);
            pipeline_ = {};
        }

        ctx_ = nullptr;
        dm_ = nullptr;
        sc_ = nullptr;
        gs_set3_layout_ = VK_NULL_HANDLE;
    }

    void GaussianSplatCullProjectPass::create_pipeline() {
        if (!ctx_ || !dm_ || !sc_ || gs_set3_layout_ == VK_NULL_HANDLE) {
            return;
        }

        const auto spirv = sc_->compile_from_file("gs/cull_project.comp", rhi::ShaderStage::Compute);
        if (spirv.empty()) {
            spdlog::warn("GaussianSplatCullProjectPass: shader compilation failed, keeping previous pipeline");
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
            .size = sizeof(GSPushConstants),
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
