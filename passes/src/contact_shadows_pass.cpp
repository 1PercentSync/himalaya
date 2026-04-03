/**
 * @file contact_shadows_pass.cpp
 * @brief ContactShadowsPass implementation — pipeline creation and per-frame compute dispatch.
 */

#include <himalaya/passes/contact_shadows_pass.h>

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
    // ---- Push constants (must match contact_shadows.comp layout) ----

    struct ContactShadowPushConstants {
        uint32_t step_count;
        float max_distance;
        float base_thickness;
    };

    static_assert(sizeof(ContactShadowPushConstants) == 12);

    // ---- Workgroup size (must match contact_shadows.comp local_size) ----

    constexpr uint32_t kWorkgroupSize = 8;

    // ---- Init / Destroy ----

    void ContactShadowsPass::setup(rhi::Context &ctx,
                                   rhi::ResourceManager &rm,
                                   rhi::DescriptorManager &dm,
                                   rhi::ShaderCompiler &sc) {
        ctx_ = &ctx;
        rm_ = &rm;
        dm_ = &dm;
        sc_ = &sc;

        // Create Set 3 push descriptor layout: binding 0 = storage image (contact_shadow_mask output)
        // Depth is read from Set 2 binding 1 (rt_depth_resolved), no push descriptor needed.
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

    void ContactShadowsPass::rebuild_pipelines() {
        create_pipeline();
    }

    void ContactShadowsPass::destroy() {
        pipeline_.destroy(ctx_->device);
        if (set3_layout_ != VK_NULL_HANDLE) {
            vkDestroyDescriptorSetLayout(ctx_->device, set3_layout_, nullptr);
            set3_layout_ = VK_NULL_HANDLE;
        }
    }

    // ---- Pipeline creation ----

    void ContactShadowsPass::create_pipeline() {
        const auto spirv = sc_->compile_from_file("contact_shadows.comp", rhi::ShaderStage::Compute);
        if (spirv.empty()) {
            spdlog::warn("ContactShadowsPass: shader compilation failed, keeping previous pipeline");
            return;
        }

        if (pipeline_.pipeline != VK_NULL_HANDLE) {
            pipeline_.destroy(ctx_->device);
        }

        // ReSharper disable once CppLocalVariableMayBeConst
        VkShaderModule module = rhi::create_shader_module(ctx_->device, spirv);

        const auto set_layouts = dm_->get_dispatch_set_layouts(set3_layout_);

        const VkPushConstantRange push_range{
            .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
            .offset = 0,
            .size = sizeof(ContactShadowPushConstants),
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

    void ContactShadowsPass::record(framework::RenderGraph &rg,
                                    const framework::FrameContext &ctx) const {
        // Declare resource usage: read depth, write contact_shadow_mask
        const std::array resources = {
            framework::RGResourceUsage{
                ctx.depth,
                framework::RGAccessType::Read,
                framework::RGStage::Compute,
            },
            framework::RGResourceUsage{
                ctx.contact_shadow_mask,
                framework::RGAccessType::Write,
                framework::RGStage::Compute,
            },
        };

        rg.add_pass("Contact Shadows", resources,
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

                         // Push Set 3 binding 0: contact_shadow_mask storage image output
                         // Depth is read from Set 2 binding 1 (rt_depth_resolved).
                         const auto mask_handle = rg.get_image(ctx.contact_shadow_mask);
                         cmd.push_storage_image(*rm_, pipeline_.layout, 3, 0, mask_handle);

                         // Push constants: contact shadow parameters
                         const ContactShadowPushConstants pc{
                             .step_count = ctx.contact_shadow_config->step_count,
                             .max_distance = ctx.contact_shadow_config->max_distance,
                             .base_thickness = ctx.contact_shadow_config->base_thickness,
                         };
                         cmd.push_constants(pipeline_.layout, VK_SHADER_STAGE_COMPUTE_BIT,
                                            &pc, sizeof(pc));

                         // Dispatch: ceil(width/8) x ceil(height/8)
                         const auto &mask_image = rm_->get_image(mask_handle);
                         const uint32_t gx = (mask_image.desc.width + kWorkgroupSize - 1) / kWorkgroupSize;
                         const uint32_t gy = (mask_image.desc.height + kWorkgroupSize - 1) / kWorkgroupSize;
                         cmd.dispatch(gx, gy, 1);
                     });
    }
} // namespace himalaya::passes
