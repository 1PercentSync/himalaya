/**
 * @file lightmap_baker_pass.cpp
 * @brief LightmapBakerPass implementation — RT pipeline creation, per-instance
 *        image management, and per-frame trace_rays dispatch with push descriptors.
 */

#include <himalaya/passes/lightmap_baker_pass.h>

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
    // ---- Push constants (must match pt_common.glsl RAYGEN_SHADER layout) ----

    struct PTPushConstants {
        uint32_t max_bounces;
        uint32_t sample_count;
        uint32_t frame_seed;
        uint32_t blue_noise_index;
        float max_clamp;
        uint32_t env_sampling;
        uint32_t directional_lights;
        uint32_t emissive_light_count;
        uint32_t lod_max_level;
        uint32_t lightmap_width;
        uint32_t lightmap_height;
        float probe_pos_x;
        float probe_pos_y;
        float probe_pos_z;
        uint32_t face_index;
    };

    static_assert(sizeof(PTPushConstants) == 60);

    // ---- Init / Destroy ----

    void LightmapBakerPass::setup(rhi::Context &ctx,
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

        // Create nearest-clamp sampler for position/normal map reads
        VkSamplerCreateInfo sampler_ci{};
        sampler_ci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        sampler_ci.magFilter = VK_FILTER_NEAREST;
        sampler_ci.minFilter = VK_FILTER_NEAREST;
        sampler_ci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sampler_ci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler_ci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler_ci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;

        VK_CHECK(vkCreateSampler(ctx_->device, &sampler_ci, nullptr, &nearest_sampler_));
        ctx_->set_debug_name(VK_OBJECT_TYPE_SAMPLER,
                             reinterpret_cast<uint64_t>(nearest_sampler_),
                             "lightmap_baker_nearest_sampler");

        // Create Set 3 push descriptor layout:
        //   binding 0 = accumulation (storage image)
        //   binding 1 = aux albedo (storage image)
        //   binding 2 = aux normal (storage image)
        //   binding 3 = Sobol direction numbers (SSBO)
        //   binding 4 = position map (combined image sampler)
        //   binding 5 = normal map (combined image sampler)
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
            VkDescriptorSetLayoutBinding{
                .binding = 4,
                .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                .descriptorCount = 1,
                .stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR,
            },
            VkDescriptorSetLayoutBinding{
                .binding = 5,
                .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                .descriptorCount = 1,
                .stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR,
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

    void LightmapBakerPass::rebuild_pipelines() {
        create_pipeline();
    }

    void LightmapBakerPass::destroy() {
        if (!ctx_) {
            return;
        }

        rt_pipeline_.destroy(ctx_->device, ctx_->allocator);

        if (nearest_sampler_ != VK_NULL_HANDLE) {
            vkDestroySampler(ctx_->device, nearest_sampler_, nullptr);
            nearest_sampler_ = VK_NULL_HANDLE;
        }

        if (set3_layout_ != VK_NULL_HANDLE) {
            vkDestroyDescriptorSetLayout(ctx_->device, set3_layout_, nullptr);
            set3_layout_ = VK_NULL_HANDLE;
        }
    }

    // ---- Pipeline creation ----

    void LightmapBakerPass::create_pipeline() {
        // Compile lightmap baker raygen + shared closesthit/miss/anyhit
        const auto rgen_spirv = sc_->compile_from_file(
            "rt/lightmap_baker.rgen", rhi::ShaderStage::RayGen);
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
            spdlog::warn("LightmapBakerPass: shader compilation failed, keeping previous pipeline");
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

    void LightmapBakerPass::record(framework::RenderGraph &rg,
                                   const framework::FrameContext &ctx) {
        // Import baker images into RG (per-instance, set via set_baker_images)
        auto rg_accum = rg.import_image(
            "baker_accumulation", accumulation_,
            VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL);
        auto rg_aux_albedo = rg.import_image(
            "baker_aux_albedo", aux_albedo_,
            VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL);
        auto rg_aux_normal = rg.import_image(
            "baker_aux_normal", aux_normal_,
            VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL);
        auto rg_pos_map = rg.import_image(
            "baker_position_map", position_map_,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        auto rg_nrm_map = rg.import_image(
            "baker_normal_map", normal_map_,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

        // Declare resource usage
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
            framework::RGResourceUsage{
                rg_pos_map,
                framework::RGAccessType::Read,
                framework::RGStage::RayTracing,
            },
            framework::RGResourceUsage{
                rg_nrm_map,
                framework::RGAccessType::Read,
                framework::RGStage::RayTracing,
            },
        };

        // Capture current accumulation state for the lambda
        const uint32_t current_sample = sample_count_;
        const uint32_t current_seed = frame_seed_;
        const uint32_t lm_w = lightmap_width_;
        const uint32_t lm_h = lightmap_height_;

        rg.add_pass("Lightmap Baker", resources,
                     [this, &rg, &ctx, current_sample, current_seed, lm_w, lm_h,
                      rg_accum, rg_aux_albedo, rg_aux_normal, rg_pos_map, rg_nrm_map](
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

                         // Push Set 3: storage images + Sobol SSBO + sampled maps
                         const auto accum_handle = rg.get_image(rg_accum);
                         const auto albedo_handle = rg.get_image(rg_aux_albedo);
                         const auto normal_handle = rg.get_image(rg_aux_normal);
                         const auto pos_handle = rg.get_image(rg_pos_map);
                         const auto nrm_handle = rg.get_image(rg_nrm_map);

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

                         VkDescriptorImageInfo pos_map_info{
                             .sampler = nearest_sampler_,
                             .imageView = rm_->get_image(pos_handle).view,
                             .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                         };
                         VkDescriptorImageInfo nrm_map_info{
                             .sampler = nearest_sampler_,
                             .imageView = rm_->get_image(nrm_handle).view,
                             .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                         };

                         const std::array<VkWriteDescriptorSet, 6> writes = {{
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
                             {
                                 .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                 .dstBinding = 4,
                                 .descriptorCount = 1,
                                 .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                 .pImageInfo = &pos_map_info,
                             },
                             {
                                 .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                 .dstBinding = 5,
                                 .descriptorCount = 1,
                                 .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                 .pImageInfo = &nrm_map_info,
                             },
                         }};

                         cmd.push_rt_descriptor_set(rt_pipeline_.layout, 3, writes);

                         // Push constants — baker always disables firefly clamping (max_clamp = 0)
                         const PTPushConstants pc{
                             .max_bounces = max_bounces_,
                             .sample_count = current_sample,
                             .frame_seed = current_seed,
                             .blue_noise_index = blue_noise_index_,
                             .max_clamp = 0.0f,
                             .env_sampling = env_sampling_ ? 1u : 0u,
                             .directional_lights = 0u,
                             .emissive_light_count = emissive_light_count_,
                             .lod_max_level = lod_max_level_,
                             .lightmap_width = lm_w,
                             .lightmap_height = lm_h,
                             .probe_pos_x = 0.0f,
                             .probe_pos_y = 0.0f,
                             .probe_pos_z = 0.0f,
                             .face_index = 0,
                         };
                         cmd.push_constants(
                             rt_pipeline_.layout,
                             VK_SHADER_STAGE_RAYGEN_BIT_KHR |
                                 VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR |
                                 VK_SHADER_STAGE_ANY_HIT_BIT_KHR,
                             &pc, sizeof(pc));

                         // Dispatch trace_rays at lightmap resolution
                         cmd.trace_rays(rt_pipeline_, lm_w, lm_h);
                     });

        // Advance accumulation state after recording
        ++sample_count_;
        ++frame_seed_;
    }

    // ---- Baker image configuration ----

    void LightmapBakerPass::set_baker_images(const rhi::ImageHandle accumulation,
                                             const rhi::ImageHandle aux_albedo,
                                             const rhi::ImageHandle aux_normal,
                                             const rhi::ImageHandle position_map,
                                             const rhi::ImageHandle normal_map,
                                             const uint32_t width,
                                             const uint32_t height) {
        accumulation_ = accumulation;
        aux_albedo_ = aux_albedo;
        aux_normal_ = aux_normal;
        position_map_ = position_map;
        normal_map_ = normal_map;
        lightmap_width_ = width;
        lightmap_height_ = height;
    }

    // ---- Accumulation management ----

    void LightmapBakerPass::reset_accumulation() {
        sample_count_ = 0;
    }

    uint32_t LightmapBakerPass::sample_count() const {
        return sample_count_;
    }

    void LightmapBakerPass::set_max_bounces(const uint32_t v) {
        max_bounces_ = v;
    }

    void LightmapBakerPass::set_env_sampling(const bool v) {
        env_sampling_ = v;
    }

    void LightmapBakerPass::set_emissive_light_count(const uint32_t v) {
        emissive_light_count_ = v;
    }

} // namespace himalaya::passes
