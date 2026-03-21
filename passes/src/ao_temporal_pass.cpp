/**
 * @file ao_temporal_pass.cpp
 * @brief AOTemporalPass implementation — pipeline creation and per-frame compute dispatch.
 */

#include <himalaya/passes/ao_temporal_pass.h>

#include <himalaya/framework/frame_context.h>
#include <himalaya/framework/render_graph.h>
#include <himalaya/framework/scene_data.h>
#include <himalaya/rhi/commands.h>
#include <himalaya/rhi/context.h>
#include <himalaya/rhi/descriptors.h>
#include <himalaya/rhi/resources.h>
#include <himalaya/rhi/shader.h>

#include <array>

#include <spdlog/spdlog.h>

namespace himalaya::passes {
    // ---- Push constants (must match ao_temporal.comp layout) ----

    struct AOTemporalPushConstants {
        float temporal_blend;
    };

    static_assert(sizeof(AOTemporalPushConstants) == 4);

    // ---- Workgroup size (must match ao_temporal.comp local_size) ----

    constexpr uint32_t kWorkgroupSize = 8;

    // ---- Init / Destroy ----

    void AOTemporalPass::setup(rhi::Context &ctx,
                               rhi::ResourceManager &rm,
                               rhi::DescriptorManager &dm,
                               rhi::ShaderCompiler &sc,
                               const rhi::SamplerHandle nearest_sampler) {
        ctx_ = &ctx;
        rm_ = &rm;
        dm_ = &dm;
        sc_ = &sc;
        nearest_sampler_ = nearest_sampler;

        // Create Set 3 push descriptor layout:
        //   binding 0 = storage image (ao_filtered output)
        //   binding 1 = combined image sampler (ao_noisy input)
        //   binding 2 = combined image sampler (ao_history input)
        //   binding 3 = combined image sampler (depth_prev input)
        const std::array bindings = {
            VkDescriptorSetLayoutBinding{
                .binding = 0,
                .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                .descriptorCount = 1,
                .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
            },
            VkDescriptorSetLayoutBinding{
                .binding = 1,
                .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                .descriptorCount = 1,
                .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
            },
            VkDescriptorSetLayoutBinding{
                .binding = 2,
                .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                .descriptorCount = 1,
                .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
            },
            VkDescriptorSetLayoutBinding{
                .binding = 3,
                .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
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

    void AOTemporalPass::rebuild_pipelines() {
        create_pipeline();
    }

    void AOTemporalPass::destroy() {
        pipeline_.destroy(ctx_->device);
        if (set3_layout_ != VK_NULL_HANDLE) {
            vkDestroyDescriptorSetLayout(ctx_->device, set3_layout_, nullptr);
            set3_layout_ = VK_NULL_HANDLE;
        }
    }

    // ---- Pipeline creation ----

    void AOTemporalPass::create_pipeline() {
        const auto spirv = sc_->compile_from_file("ao_temporal.comp", rhi::ShaderStage::Compute);
        if (spirv.empty()) {
            spdlog::warn("AOTemporalPass: shader compilation failed, keeping previous pipeline");
            return;
        }

        if (pipeline_.pipeline != VK_NULL_HANDLE) {
            pipeline_.destroy(ctx_->device);
        }

        // ReSharper disable once CppLocalVariableMayBeConst
        VkShaderModule module = rhi::create_shader_module(ctx_->device, spirv);

        const auto set_layouts = dm_->get_compute_set_layouts(set3_layout_);

        const VkPushConstantRange push_range{
            .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
            .offset = 0,
            .size = sizeof(AOTemporalPushConstants),
        };

        const rhi::ComputePipelineDesc desc{
            .compute_shader = module,
            .descriptor_set_layouts = set_layouts,
            .push_constant_ranges = {push_range},
        };

        pipeline_ = rhi::create_compute_pipeline(ctx_->device, desc);

        vkDestroyShaderModule(ctx_->device, module, nullptr);
    }

    // ---- Per-frame recording ----

    void AOTemporalPass::record(framework::RenderGraph &rg,
                                const framework::FrameContext &ctx) const {
        // Declare resource usage:
        //   read: depth (Set 2 for reprojection), ao_noisy, ao_history, depth_prev
        //   write: ao_filtered
        const std::array resources = {
            framework::RGResourceUsage{
                ctx.depth,
                framework::RGAccessType::Read,
                framework::RGStage::Compute,
            },
            framework::RGResourceUsage{
                ctx.ao_noisy,
                framework::RGAccessType::Read,
                framework::RGStage::Compute,
            },
            framework::RGResourceUsage{
                ctx.ao_history,
                framework::RGAccessType::Read,
                framework::RGStage::Compute,
            },
            framework::RGResourceUsage{
                ctx.depth_prev,
                framework::RGAccessType::Read,
                framework::RGStage::Compute,
            },
            framework::RGResourceUsage{
                ctx.ao_filtered,
                framework::RGAccessType::Write,
                framework::RGStage::Compute,
            },
        };

        rg.add_pass("AO Temporal", resources,
                     [this, &rg, &ctx](const rhi::CommandBuffer &cmd) {
                         cmd.bind_compute_pipeline(pipeline_);

                         // Bind global descriptor sets (Set 0-2)
                         const std::array sets = {
                             dm_->get_set0(ctx.frame_index),
                             dm_->get_set1(),
                             dm_->get_set2(ctx.frame_index),
                         };
                         cmd.bind_compute_descriptor_sets(
                             pipeline_.layout, 0,
                             sets.data(), static_cast<uint32_t>(sets.size()));

                         // Push Set 3 binding 0: ao_filtered storage image output
                         const auto ao_filtered_handle = rg.get_image(ctx.ao_filtered);
                         cmd.push_storage_image(*rm_, pipeline_.layout, 3, 0, ao_filtered_handle);

                         // Push Set 3 binding 1: ao_noisy sampled input
                         const auto ao_noisy_handle = rg.get_image(ctx.ao_noisy);
                         cmd.push_sampled_image(*rm_, pipeline_.layout, 3, 1,
                                               ao_noisy_handle, nearest_sampler_);

                         // Push Set 3 binding 2: ao_history sampled input
                         const auto ao_history_handle = rg.get_image(ctx.ao_history);
                         cmd.push_sampled_image(*rm_, pipeline_.layout, 3, 2,
                                               ao_history_handle, nearest_sampler_);

                         // Push Set 3 binding 3: depth_prev sampled input
                         const auto depth_prev_handle = rg.get_image(ctx.depth_prev);
                         cmd.push_sampled_image(*rm_, pipeline_.layout, 3, 3,
                                               depth_prev_handle, nearest_sampler_);

                         // Push constants: temporal blend factor
                         const AOTemporalPushConstants pc{
                             .temporal_blend = ctx.ao_history_valid
                                                  ? ctx.ao_config->temporal_blend
                                                  : 0.0f,
                         };
                         cmd.push_constants(pipeline_.layout, VK_SHADER_STAGE_COMPUTE_BIT,
                                            &pc, sizeof(pc));

                         // Dispatch: ceil(width/8) x ceil(height/8)
                         const auto &ao_image = rm_->get_image(ao_filtered_handle);
                         const uint32_t gx = (ao_image.desc.width + kWorkgroupSize - 1) / kWorkgroupSize;
                         const uint32_t gy = (ao_image.desc.height + kWorkgroupSize - 1) / kWorkgroupSize;
                         cmd.dispatch(gx, gy, 1);
                     });
    }
} // namespace himalaya::passes
