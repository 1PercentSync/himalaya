/**
 * @file gtao_pass.cpp
 * @brief GTAOPass implementation — pipeline creation and per-frame compute dispatch.
 */

#include <himalaya/passes/gtao_pass.h>

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
    // ---- Push constants (must match gtao.comp layout) ----

    struct GTAOPushConstants {
        float radius;
        uint32_t directions;
        uint32_t steps_per_dir;
        float bias;
        float intensity;
        uint32_t frame_index;
    };

    static_assert(sizeof(GTAOPushConstants) == 24);

    // ---- Workgroup size (must match gtao.comp local_size) ----

    constexpr uint32_t kWorkgroupSize = 8;

    // ---- Init / Destroy ----

    void GTAOPass::setup(rhi::Context &ctx,
                         rhi::ResourceManager &rm,
                         rhi::DescriptorManager &dm,
                         rhi::ShaderCompiler &sc) {
        ctx_ = &ctx;
        rm_ = &rm;
        dm_ = &dm;
        sc_ = &sc;

        // Create Set 3 push descriptor layout: binding 0 = storage image (ao_noisy output)
        const VkDescriptorSetLayoutBinding binding{
            .binding = 0,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
        };

        VkDescriptorSetLayoutCreateInfo layout_ci{};
        layout_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        layout_ci.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT;
        layout_ci.bindingCount = 1;
        layout_ci.pBindings = &binding;

        VK_CHECK(vkCreateDescriptorSetLayout(ctx_->device, &layout_ci, nullptr, &set3_layout_));

        create_pipeline();
    }

    void GTAOPass::rebuild_pipelines() {
        create_pipeline();
    }

    void GTAOPass::destroy() {
        pipeline_.destroy(ctx_->device);
        if (set3_layout_ != VK_NULL_HANDLE) {
            vkDestroyDescriptorSetLayout(ctx_->device, set3_layout_, nullptr);
            set3_layout_ = VK_NULL_HANDLE;
        }
    }

    // ---- Pipeline creation ----

    void GTAOPass::create_pipeline() {
        const auto spirv = sc_->compile_from_file("gtao.comp", rhi::ShaderStage::Compute);
        if (spirv.empty()) {
            spdlog::warn("GTAOPass: shader compilation failed, keeping previous pipeline");
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
            .size = sizeof(GTAOPushConstants),
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

    void GTAOPass::record(framework::RenderGraph &rg,
                          const framework::FrameContext &ctx) const {
        // Declare resource usage: read depth + normal, write ao_noisy
        const std::array resources = {
            framework::RGResourceUsage{
                ctx.depth,
                framework::RGAccessType::Read,
                framework::RGStage::Compute,
            },
            framework::RGResourceUsage{
                ctx.normal,
                framework::RGAccessType::Read,
                framework::RGStage::Compute,
            },
            framework::RGResourceUsage{
                ctx.ao_noisy,
                framework::RGAccessType::Write,
                framework::RGStage::Compute,
            },
        };

        rg.add_pass("GTAO", resources,
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

                         // Push Set 3: ao_noisy storage image output
                         const auto ao_noisy_handle = rg.get_image(ctx.ao_noisy);
                         cmd.push_storage_image(*rm_, pipeline_.layout, 3, 0, ao_noisy_handle);

                         // Push constants: AO parameters
                         const GTAOPushConstants pc{
                             .radius = ctx.ao_config->radius,
                             .directions = ctx.ao_config->directions,
                             .steps_per_dir = ctx.ao_config->steps_per_dir,
                             .bias = ctx.ao_config->bias,
                             .intensity = ctx.ao_config->intensity,
                             .frame_index = ctx.frame_index,
                         };
                         cmd.push_constants(pipeline_.layout, VK_SHADER_STAGE_COMPUTE_BIT,
                                            &pc, sizeof(pc));

                         // Dispatch: ceil(width/8) x ceil(height/8)
                         const auto &ao_image = rm_->get_image(ao_noisy_handle);
                         const uint32_t gx = (ao_image.desc.width + kWorkgroupSize - 1) / kWorkgroupSize;
                         const uint32_t gy = (ao_image.desc.height + kWorkgroupSize - 1) / kWorkgroupSize;
                         cmd.dispatch(gx, gy, 1);
                     });
    }
} // namespace himalaya::passes
