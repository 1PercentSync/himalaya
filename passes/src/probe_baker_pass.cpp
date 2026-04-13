/**
 * @file probe_baker_pass.cpp
 * @brief ProbeBakerPass implementation — RT pipeline creation, per-face view
 *        management, and per-frame 6-face trace_rays dispatch with push descriptors.
 */

#include <himalaya/passes/probe_baker_pass.h>
#include <himalaya/passes/pt_push_constants.h>

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
    // ---- Init / Destroy ----

    void ProbeBakerPass::setup(rhi::Context &ctx,
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
        //   binding 0 = accumulation face view (storage image)
        //   binding 1 = aux albedo face view (storage image)
        //   binding 2 = aux normal face view (storage image)
        //   binding 3 = Sobol direction numbers (SSBO)
        constexpr VkShaderStageFlags rt_stages =
                VK_SHADER_STAGE_RAYGEN_BIT_KHR |
                VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR |
                VK_SHADER_STAGE_MISS_BIT_KHR |
                VK_SHADER_STAGE_ANY_HIT_BIT_KHR;

        constexpr std::array bindings = {
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

    void ProbeBakerPass::rebuild_pipelines() {
        create_pipeline();
    }

    void ProbeBakerPass::destroy() {
        if (!ctx_) {
            return;
        }

        destroy_face_views();
        rt_pipeline_.destroy(ctx_->device, ctx_->allocator);

        if (set3_layout_ != VK_NULL_HANDLE) {
            vkDestroyDescriptorSetLayout(ctx_->device, set3_layout_, nullptr);
            set3_layout_ = VK_NULL_HANDLE;
        }
    }

    // ---- Pipeline creation ----

    void ProbeBakerPass::create_pipeline() {
        // Compile probe baker raygen + shared closesthit/miss/anyhit
        const auto rgen_spirv = sc_->compile_from_file(
            "rt/probe_baker.rgen", rhi::ShaderStage::RayGen);
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
            spdlog::warn("ProbeBakerPass: shader compilation failed, keeping previous pipeline");
            return;
        }

        // Destroy old pipeline if rebuilding
        if (rt_pipeline_.pipeline != VK_NULL_HANDLE) {
            rt_pipeline_.destroy(ctx_->device, ctx_->allocator);
        }

        // ReSharper disable CppLocalVariableMayBeConst
        VkShaderModule rgen_module = rhi::create_shader_module(ctx_->device, rgen_spirv);
        VkShaderModule chit_module = rhi::create_shader_module(ctx_->device, chit_spirv);
        VkShaderModule miss_module = rhi::create_shader_module(ctx_->device, miss_spirv);
        VkShaderModule shadow_miss_module = rhi::create_shader_module(ctx_->device, shadow_miss_spirv);
        VkShaderModule ahit_module = rhi::create_shader_module(ctx_->device, ahit_spirv);
        // ReSharper restore CppLocalVariableMayBeConst

        const auto set_layouts = dm_->get_dispatch_set_layouts(set3_layout_);

        constexpr VkPushConstantRange push_range{
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

    void ProbeBakerPass::record(framework::RenderGraph &rg,
                                const framework::FrameContext &ctx,
                                const framework::RGResourceId rg_accum,
                                const framework::RGResourceId rg_aux_albedo,
                                const framework::RGResourceId rg_aux_normal) {
        if (rt_pipeline_.pipeline == VK_NULL_HANDLE) {
            return;
        }

        // Declare resource usage — all 6 faces access the same parent images
        const std::array resources = {
            framework::RGResourceUsage{
                rg_accum,
                framework::RGAccessType::ReadWrite,
                framework::RGStage::RayTracing,
            },
            framework::RGResourceUsage{
                rg_aux_albedo,
                framework::RGAccessType::Write,
                framework::RGStage::RayTracing,
            },
            framework::RGResourceUsage{
                rg_aux_normal,
                framework::RGAccessType::Write,
                framework::RGStage::RayTracing,
            },
        };

        // Capture current state for the lambda
        const uint32_t current_sample = sample_count_;
        const uint32_t current_seed = frame_seed_;
        const uint32_t res = face_res_;

        rg.add_pass("Probe Baker", resources,
                    [this, &ctx, current_sample, current_seed, res](
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

                        // Sobol SSBO descriptor (shared across all 6 face dispatches)
                        const auto &sobol = rm_->get_buffer(sobol_buffer_);
                        VkDescriptorBufferInfo sobol_info{
                            .buffer = sobol.buffer,
                            .offset = 0,
                            .range = sobol.desc.size,
                        };

                        // 6 dispatches — one per cubemap face
                        for (uint32_t face = 0; face < kFaceCount; ++face) {
                            // Per-face image views for push descriptors
                            VkDescriptorImageInfo accum_info{
                                .imageView = accum_face_views_[face],
                                .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
                            };
                            VkDescriptorImageInfo albedo_info{
                                .imageView = aux_albedo_face_views_[face],
                                .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
                            };
                            VkDescriptorImageInfo normal_info{
                                .imageView = aux_normal_face_views_[face],
                                .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
                            };

                            const std::array<VkWriteDescriptorSet, 4> writes = {
                                {
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
                                }
                            };

                            cmd.push_rt_descriptor_set(rt_pipeline_.layout, 3, writes);

                            // Push constants — per-face: face_index varies, rest shared
                            const PTPushConstants pc{
                                .max_bounces = max_bounces_,
                                .sample_count = current_sample,
                                .frame_seed = current_seed,
                                .blue_noise_index = blue_noise_index_,
                                .max_clamp = 0.0f,
                                .env_sampling = env_sampling_ ? 1u : 0u,
                                .directional_lights = 0u,
                                .emissive_light_count = emissive_light_count_,
                                .lod_max_level = 0u,
                                .lightmap_width = 0u,
                                .lightmap_height = 0u,
                                .probe_pos_x = probe_pos_x_,
                                .probe_pos_y = probe_pos_y_,
                                .probe_pos_z = probe_pos_z_,
                                .face_index = face,
                            };
                            cmd.push_constants(
                                rt_pipeline_.layout,
                                VK_SHADER_STAGE_RAYGEN_BIT_KHR |
                                VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR |
                                VK_SHADER_STAGE_ANY_HIT_BIT_KHR,
                                &pc, sizeof(pc));

                            cmd.trace_rays(rt_pipeline_, res, res);
                        }
                    });

        // Advance accumulation state after recording (once per frame, not per face)
        ++sample_count_;
        ++frame_seed_;
    }

    // ---- Probe image configuration ----

    void ProbeBakerPass::set_probe_images(const rhi::ImageHandle accumulation,
                                          const rhi::ImageHandle aux_albedo,
                                          const rhi::ImageHandle aux_normal,
                                          const uint32_t face_res) {
        // Destroy previous views if any
        destroy_face_views();

        accumulation_ = accumulation;
        aux_albedo_ = aux_albedo;
        aux_normal_ = aux_normal;
        face_res_ = face_res;

        // Create 18 per-face 2D views (6 per image × 3 images)
        for (uint32_t f = 0; f < kFaceCount; ++f) {
            char name[64];

            std::snprintf(name, sizeof(name), "probe_accum_face%u", f);
            accum_face_views_[f] = rm_->create_layer_view(accumulation, f, name);

            std::snprintf(name, sizeof(name), "probe_aux_albedo_face%u", f);
            aux_albedo_face_views_[f] = rm_->create_layer_view(aux_albedo, f, name);

            std::snprintf(name, sizeof(name), "probe_aux_normal_face%u", f);
            aux_normal_face_views_[f] = rm_->create_layer_view(aux_normal, f, name);
        }
    }

    void ProbeBakerPass::destroy_face_views() {
        if (!rm_) {
            return;
        }

        for (uint32_t f = 0; f < kFaceCount; ++f) {
            rm_->destroy_layer_view(accum_face_views_[f]);
            accum_face_views_[f] = VK_NULL_HANDLE;

            rm_->destroy_layer_view(aux_albedo_face_views_[f]);
            aux_albedo_face_views_[f] = VK_NULL_HANDLE;

            rm_->destroy_layer_view(aux_normal_face_views_[f]);
            aux_normal_face_views_[f] = VK_NULL_HANDLE;
        }
    }

    // ---- Probe position ----

    void ProbeBakerPass::set_probe_position(const float x, const float y, const float z) {
        probe_pos_x_ = x;
        probe_pos_y_ = y;
        probe_pos_z_ = z;
    }

    // ---- Accumulation management ----

    void ProbeBakerPass::reset_accumulation() {
        sample_count_ = 0;
    }

    uint32_t ProbeBakerPass::sample_count() const {
        return sample_count_;
    }

    void ProbeBakerPass::set_max_bounces(const uint32_t v) {
        max_bounces_ = v;
    }

    void ProbeBakerPass::set_env_sampling(const bool v) {
        env_sampling_ = v;
    }

    void ProbeBakerPass::set_emissive_light_count(const uint32_t v) {
        emissive_light_count_ = v;
    }
} // namespace himalaya::passes
