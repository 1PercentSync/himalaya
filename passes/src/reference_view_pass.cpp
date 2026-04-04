/**
 * @file reference_view_pass.cpp
 * @brief ReferenceViewPass implementation — RT pipeline creation and per-frame
 *        trace_rays dispatch with push descriptors.
 */

#include <himalaya/passes/reference_view_pass.h>

#include <himalaya/framework/frame_context.h>
#include <himalaya/framework/render_graph.h>
#include <himalaya/rhi/commands.h>
#include <himalaya/rhi/context.h>
#include <himalaya/rhi/descriptors.h>
#include <himalaya/rhi/resources.h>
#include <himalaya/rhi/shader.h>

#include <array>

#include <spdlog/spdlog.h>

namespace himalaya::passes {
    // ---- Push constants (must match reference_view.rgen / closesthit.rchit layout) ----

    struct PTPushConstants {
        uint32_t max_bounces;
        uint32_t sample_count;
        uint32_t frame_seed;
        uint32_t blue_noise_index;
        float max_clamp;
    };

    static_assert(sizeof(PTPushConstants) == 20);

    // ---- Default PT parameters (Step 10 will expose via ImGui) ----

    constexpr uint32_t kDefaultMaxBounces = 8;
    constexpr float kDefaultMaxClamp = 10.0f;

    // ---- Init / Destroy ----

    void ReferenceViewPass::setup(rhi::Context &ctx,
                                  rhi::ResourceManager &rm,
                                  rhi::DescriptorManager &dm,
                                  rhi::ShaderCompiler &sc,
                                  const rhi::BufferHandle sobol_buffer,
                                  const uint32_t blue_noise_index) {
        ctx_ = &ctx;
        rm_ = &rm;
        dm_ = &dm;
        sc_ = &sc;
        sobol_buffer_ = sobol_buffer;
        blue_noise_index_ = blue_noise_index;

        // Create Set 3 push descriptor layout:
        //   binding 0 = accumulation (storage image)
        //   binding 1 = aux albedo (storage image)
        //   binding 2 = aux normal (storage image)
        //   binding 3 = Sobol direction numbers (SSBO)
        constexpr VkShaderStageFlags rt_stages =
            VK_SHADER_STAGE_RAYGEN_BIT_KHR |
            VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR |
            VK_SHADER_STAGE_MISS_BIT_KHR |
            VK_SHADER_STAGE_ANY_HIT_BIT_KHR;

        const std::array bindings = {
            VkDescriptorSetLayoutBinding{
                .binding = 0,
                .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                .descriptorCount = 1,
                .stageFlags = rt_stages,
            },
            VkDescriptorSetLayoutBinding{
                .binding = 1,
                .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                .descriptorCount = 1,
                .stageFlags = rt_stages,
            },
            VkDescriptorSetLayoutBinding{
                .binding = 2,
                .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                .descriptorCount = 1,
                .stageFlags = rt_stages,
            },
            VkDescriptorSetLayoutBinding{
                .binding = 3,
                .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                .descriptorCount = 1,
                .stageFlags = rt_stages,
            },
        };

        VkDescriptorSetLayoutCreateInfo layout_ci{};
        layout_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        layout_ci.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT;
        layout_ci.bindingCount = static_cast<uint32_t>(bindings.size());
        layout_ci.pBindings = bindings.data();

        VK_CHECK(vkCreateDescriptorSetLayout(ctx_->device, &layout_ci, nullptr, &set3_layout_));

        // Create RT pipeline
        create_pipeline();
    }

    void ReferenceViewPass::rebuild_pipelines() {
        create_pipeline();
    }

    void ReferenceViewPass::destroy() {
        rt_pipeline_.destroy(ctx_->device, ctx_->allocator);

        if (set3_layout_ != VK_NULL_HANDLE) {
            vkDestroyDescriptorSetLayout(ctx_->device, set3_layout_, nullptr);
            set3_layout_ = VK_NULL_HANDLE;
        }
    }

    // ---- Pipeline creation ----

    void ReferenceViewPass::create_pipeline() {
        // Compile all RT shader stages
        const auto rgen_spirv = sc_->compile_from_file(
            "rt/reference_view.rgen", rhi::ShaderStage::RayGen);
        const auto chit_spirv = sc_->compile_from_file(
            "rt/closesthit.rchit", rhi::ShaderStage::ClosestHit);
        const auto miss_spirv = sc_->compile_from_file(
            "rt/miss.rmiss", rhi::ShaderStage::Miss);
        const auto shadow_miss_spirv = sc_->compile_from_file(
            "rt/shadow_miss.rmiss", rhi::ShaderStage::Miss);
        const auto ahit_spirv = sc_->compile_from_file(
            "rt/anyhit.rahit", rhi::ShaderStage::AnyHit);

        if (rgen_spirv.empty() || chit_spirv.empty() || miss_spirv.empty() ||
            shadow_miss_spirv.empty() || ahit_spirv.empty()) {
            spdlog::warn("ReferenceViewPass: shader compilation failed, keeping previous pipeline");
            return;
        }

        // Destroy old pipeline if rebuilding
        if (rt_pipeline_.pipeline != VK_NULL_HANDLE) {
            rt_pipeline_.destroy(ctx_->device, ctx_->allocator);
        }

        // Create shader modules
        // ReSharper disable CppLocalVariableMayBeConst
        VkShaderModule rgen_module = rhi::create_shader_module(ctx_->device, rgen_spirv);
        VkShaderModule chit_module = rhi::create_shader_module(ctx_->device, chit_spirv);
        VkShaderModule miss_module = rhi::create_shader_module(ctx_->device, miss_spirv);
        VkShaderModule shadow_miss_module = rhi::create_shader_module(ctx_->device, shadow_miss_spirv);
        VkShaderModule ahit_module = rhi::create_shader_module(ctx_->device, ahit_spirv);
        // ReSharper restore CppLocalVariableMayBeConst

        const auto set_layouts = dm_->get_dispatch_set_layouts(set3_layout_);

        const VkPushConstantRange push_range{
            .stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR |
                          VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR |
                          VK_SHADER_STAGE_ANY_HIT_BIT_KHR,
            .offset = 0,
            .size = sizeof(PTPushConstants),
        };

        const rhi::RTPipelineDesc desc{
            .raygen = rgen_module,
            .miss = miss_module,
            .shadow_miss = shadow_miss_module,
            .closesthit = chit_module,
            .anyhit = ahit_module,
            .max_recursion_depth = 1,
            .descriptor_set_layouts = set_layouts,
            .push_constant_ranges = {&push_range, 1},
        };

        rt_pipeline_ = rhi::create_rt_pipeline(*ctx_, desc);

        // Destroy shader modules (pipeline retains SPIR-V internally)
        vkDestroyShaderModule(ctx_->device, ahit_module, nullptr);
        vkDestroyShaderModule(ctx_->device, shadow_miss_module, nullptr);
        vkDestroyShaderModule(ctx_->device, miss_module, nullptr);
        vkDestroyShaderModule(ctx_->device, chit_module, nullptr);
        vkDestroyShaderModule(ctx_->device, rgen_module, nullptr);
    }

    // ---- Per-frame recording ----

    void ReferenceViewPass::record(framework::RenderGraph &rg,
                                   const framework::FrameContext &ctx) {
        // Declare resource usage:
        //   accumulation = ReadWrite (imageLoad + imageStore)
        //   aux albedo/normal = Write (imageStore only, bounce 0)
        const std::array resources = {
            framework::RGResourceUsage{
                ctx.pt_accumulation,
                framework::RGAccessType::ReadWrite,
                framework::RGStage::RayTracing,
            },
            framework::RGResourceUsage{
                ctx.pt_aux_albedo,
                framework::RGAccessType::Write,
                framework::RGStage::RayTracing,
            },
            framework::RGResourceUsage{
                ctx.pt_aux_normal,
                framework::RGAccessType::Write,
                framework::RGStage::RayTracing,
            },
        };

        // Capture current sample count and frame seed for the lambda
        const uint32_t current_sample = sample_count_;
        const uint32_t current_seed = frame_seed_;

        rg.add_pass("Reference View", resources,
                     [this, &rg, &ctx, current_sample, current_seed](
                         const rhi::CommandBuffer &cmd) {
                         cmd.bind_rt_pipeline(rt_pipeline_);

                         // Bind global descriptor sets (Set 0, Set 1, Set 2)
                         const std::array sets = {
                             dm_->get_set0(ctx.frame_index),
                             dm_->get_set1(),
                             dm_->get_set2(ctx.frame_index),
                         };
                         cmd.bind_rt_descriptor_sets(
                             rt_pipeline_.layout, 0,
                             sets.data(), static_cast<uint32_t>(sets.size()));

                         // Push Set 3: storage images + Sobol SSBO
                         const auto accum_handle = rg.get_image(ctx.pt_accumulation);
                         const auto albedo_handle = rg.get_image(ctx.pt_aux_albedo);
                         const auto normal_handle = rg.get_image(ctx.pt_aux_normal);

                         VkDescriptorImageInfo accum_info{
                             .imageView = rm_->get_image(accum_handle).view,
                             .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
                         };
                         VkDescriptorImageInfo albedo_info{
                             .imageView = rm_->get_image(albedo_handle).view,
                             .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
                         };
                         VkDescriptorImageInfo normal_info{
                             .imageView = rm_->get_image(normal_handle).view,
                             .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
                         };

                         const auto &sobol = rm_->get_buffer(sobol_buffer_);
                         VkDescriptorBufferInfo sobol_info{
                             .buffer = sobol.buffer,
                             .offset = 0,
                             .range = sobol.desc.size,
                         };

                         const std::array<VkWriteDescriptorSet, 4> writes = {{
                             {
                                 .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                 .dstBinding = 0,
                                 .descriptorCount = 1,
                                 .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                                 .pImageInfo = &accum_info,
                             },
                             {
                                 .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                 .dstBinding = 1,
                                 .descriptorCount = 1,
                                 .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                                 .pImageInfo = &albedo_info,
                             },
                             {
                                 .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                 .dstBinding = 2,
                                 .descriptorCount = 1,
                                 .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                                 .pImageInfo = &normal_info,
                             },
                             {
                                 .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                 .dstBinding = 3,
                                 .descriptorCount = 1,
                                 .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                 .pBufferInfo = &sobol_info,
                             },
                         }};

                         cmd.push_rt_descriptor_set(rt_pipeline_.layout, 3, writes);

                         // Push constants
                         const PTPushConstants pc{
                             .max_bounces = kDefaultMaxBounces,
                             .sample_count = current_sample,
                             .frame_seed = current_seed,
                             .blue_noise_index = blue_noise_index_,
                             .max_clamp = kDefaultMaxClamp,
                         };
                         cmd.push_constants(
                             rt_pipeline_.layout,
                             VK_SHADER_STAGE_RAYGEN_BIT_KHR |
                                 VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR |
                                 VK_SHADER_STAGE_ANY_HIT_BIT_KHR,
                             &pc, sizeof(pc));

                         // Dispatch trace_rays
                         const auto &accum_image = rm_->get_image(accum_handle);
                         cmd.trace_rays(rt_pipeline_,
                                        accum_image.desc.width,
                                        accum_image.desc.height);
                     });

        // Advance accumulation state after recording
        ++sample_count_;
        ++frame_seed_;
    }

    // ---- Accumulation management ----

    void ReferenceViewPass::reset_accumulation() {
        sample_count_ = 0;
    }

    uint32_t ReferenceViewPass::sample_count() const {
        return sample_count_;
    }
} // namespace himalaya::passes
