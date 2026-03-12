/**
 * @file ibl.cpp
 * @brief IBL precomputation implementation.
 *
 * Loads an equirectangular .hdr environment map, uploads it to GPU,
 * and runs four compute shader passes to produce cubemap, irradiance map,
 * prefiltered environment map, and BRDF integration LUT.
 * All work runs within a single begin_immediate()/end_immediate() scope.
 */

#include <himalaya/framework/ibl.h>

#include <himalaya/rhi/commands.h>
#include <himalaya/rhi/context.h>
#include <himalaya/rhi/descriptors.h>
#include <himalaya/rhi/pipeline.h>
#include <himalaya/rhi/resources.h>
#include <himalaya/rhi/shader.h>

#include <glm/gtc/packing.hpp>
#include <spdlog/spdlog.h>
#include <stb_image.h>

#include <cmath>
#include <vector>

namespace himalaya::framework {
    // ---- IBL image dimensions ----

    constexpr uint32_t kCubemapSize = 1024;
    constexpr uint32_t kIrradianceSize = 32;
    constexpr uint32_t kPrefilteredSize = 256;
    constexpr uint32_t kBrdfLutSize = 256;

    /** @brief Compute shader workgroup size (must match local_size in all .comp files). */
    constexpr uint32_t kLocalSize = 16;

    // ---- Helpers ----

    /**
     * Creates a VK_IMAGE_VIEW_TYPE_2D_ARRAY view for storage image writes.
     * Cubemap images need 2D_ARRAY views for image2DArray in compute shaders,
     * while their default view is CUBE (for samplerCube sampling).
     */
    // ReSharper disable once CppParameterMayBeConst
    static VkImageView create_storage_view(VkDevice device,
                                           // ReSharper disable once CppParameterMayBeConst
                                           VkImage image,
                                           const VkFormat format,
                                           const uint32_t mip_level,
                                           // ReSharper disable once CppDFAConstantParameter
                                           const uint32_t layer_count) {
        VkImageViewCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        info.image = image;
        info.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
        info.format = format;
        info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        info.subresourceRange.baseMipLevel = mip_level;
        info.subresourceRange.levelCount = 1;
        info.subresourceRange.baseArrayLayer = 0;
        info.subresourceRange.layerCount = layer_count;

        VkImageView view;
        VK_CHECK(vkCreateImageView(device, &info, nullptr, &view));
        return view;
    }

    /**
     * Records a pipeline barrier for an image layout transition (or execution dependency).
     * Convenience wrapper for single-image barriers in the immediate scope.
     */
    // ReSharper disable once CppParameterMayBeConst
    static void image_barrier(VkCommandBuffer cmd,
                              // ReSharper disable once CppParameterMayBeConst
                              VkImage image,
                              const VkPipelineStageFlags2 src_stage,
                              const VkAccessFlags2 src_access,
                              const VkPipelineStageFlags2 dst_stage,
                              const VkAccessFlags2 dst_access,
                              const VkImageLayout old_layout,
                              const VkImageLayout new_layout,
                              const uint32_t mip_count,
                              const uint32_t layer_count) {
        VkImageMemoryBarrier2 barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        barrier.srcStageMask = src_stage;
        barrier.srcAccessMask = src_access;
        barrier.dstStageMask = dst_stage;
        barrier.dstAccessMask = dst_access;
        barrier.oldLayout = old_layout;
        barrier.newLayout = new_layout;
        barrier.image = image;
        barrier.subresourceRange = {
            VK_IMAGE_ASPECT_COLOR_BIT,
            0,
            mip_count,
            0,
            layer_count
        };

        VkDependencyInfo dep{};
        dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dep.imageMemoryBarrierCount = 1;
        dep.pImageMemoryBarriers = &barrier;
        vkCmdPipelineBarrier2(cmd, &dep);
    }

    /**
     * Creates a push descriptor set layout with the VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT flag.
     */
    static VkDescriptorSetLayout create_push_layout(
        // ReSharper disable once CppParameterMayBeConst
        VkDevice device,
        const VkDescriptorSetLayoutBinding *bindings,
        const uint32_t binding_count) {
        VkDescriptorSetLayoutCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        info.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT;
        info.bindingCount = binding_count;
        info.pBindings = bindings;

        VkDescriptorSetLayout layout;
        VK_CHECK(vkCreateDescriptorSetLayout(device, &info, nullptr, &layout));
        return layout;
    }

    /**
     * Constructs a VkWriteDescriptorSet for push descriptors (dstSet = VK_NULL_HANDLE).
     */
    static VkWriteDescriptorSet push_write(const uint32_t binding,
                                           const VkDescriptorType type,
                                           const VkDescriptorImageInfo *image_info) {
        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstBinding = binding;
        write.descriptorCount = 1;
        write.descriptorType = type;
        write.pImageInfo = image_info;
        return write;
    }

    // ---- IBL::init() ----

    void IBL::init(rhi::Context &ctx,
                   rhi::ResourceManager &rm,
                   rhi::DescriptorManager &dm,
                   rhi::ShaderCompiler &sc,
                   const std::string &hdr_path) {
        ctx_ = &ctx;
        rm_ = &rm;
        dm_ = &dm;

        // ---- 1. Load .hdr file (float32 RGB) ----

        int hdr_width = 0, hdr_height = 0, hdr_channels = 0;
        float *hdr_data = stbi_loadf(hdr_path.c_str(),
                                     &hdr_width,
                                     &hdr_height,
                                     &hdr_channels,
                                     3);
        if (!hdr_data) {
            spdlog::error("IBL: failed to load HDR file: {}", hdr_path);
            return;
        }
        spdlog::info("IBL: loaded {} ({}x{}, {} channels)",
                     hdr_path, hdr_width, hdr_height, hdr_channels);

        // ---- 2. Convert float32 RGB → float16 RGBA ----
        // stbi_loadf returns float32 RGB; GPU equirect uses R16G16B16A16F.

        const auto pixel_count = static_cast<size_t>(hdr_width) * hdr_height;
        std::vector<uint16_t> rgba16(pixel_count * 4);
        for (size_t i = 0; i < pixel_count; ++i) {
            rgba16[i * 4 + 0] = glm::packHalf1x16(hdr_data[i * 3 + 0]);
            rgba16[i * 4 + 1] = glm::packHalf1x16(hdr_data[i * 3 + 1]);
            rgba16[i * 4 + 2] = glm::packHalf1x16(hdr_data[i * 3 + 2]);
            rgba16[i * 4 + 3] = glm::packHalf1x16(1.0f);
        }
        stbi_image_free(hdr_data);

        // ---- 3. Create GPU images ----

        const auto equirect = rm.create_image({
                                                  .width = static_cast<uint32_t>(hdr_width),
                                                  .height = static_cast<uint32_t>(hdr_height),
                                                  .depth = 1,
                                                  .mip_levels = 1,
                                                  .array_layers = 1,
                                                  .sample_count = 1,
                                                  .format = rhi::Format::R16G16B16A16Sfloat,
                                                  .usage = rhi::ImageUsage::Sampled | rhi::ImageUsage::TransferDst,
                                              }, "IBL Equirect Input");

        cubemap_ = rm.create_image({
                                       .width = kCubemapSize,
                                       .height = kCubemapSize,
                                       .depth = 1,
                                       .mip_levels = 1,
                                       .array_layers = 6,
                                       .sample_count = 1,
                                       .format = rhi::Format::R16G16B16A16Sfloat,
                                       .usage = rhi::ImageUsage::Sampled | rhi::ImageUsage::Storage,
                                   }, "IBL Cubemap");

        irradiance_ = rm.create_image({
                                          .width = kIrradianceSize,
                                          .height = kIrradianceSize,
                                          .depth = 1,
                                          .mip_levels = 1,
                                          .array_layers = 6,
                                          .sample_count = 1,
                                          .format = rhi::Format::B10G11R11UfloatPack32,
                                          .usage = rhi::ImageUsage::Sampled | rhi::ImageUsage::Storage,
                                      }, "IBL Irradiance");

        prefiltered_mip_count_ = static_cast<uint32_t>(
                                     std::floor(std::log2(static_cast<float>(kPrefilteredSize)))) + 1;
        prefiltered_ = rm.create_image({
                                           .width = kPrefilteredSize,
                                           .height = kPrefilteredSize,
                                           .depth = 1,
                                           .mip_levels = prefiltered_mip_count_,
                                           .array_layers = 6,
                                           .sample_count = 1,
                                           .format = rhi::Format::R16G16B16A16Sfloat,
                                           .usage = rhi::ImageUsage::Sampled | rhi::ImageUsage::Storage,
                                       }, "IBL Prefiltered");

        brdf_lut_ = rm.create_image({
                                        .width = kBrdfLutSize,
                                        .height = kBrdfLutSize,
                                        .depth = 1,
                                        .mip_levels = 1,
                                        .array_layers = 1,
                                        .sample_count = 1,
                                        .format = rhi::Format::R16G16Unorm,
                                        .usage = rhi::ImageUsage::Sampled | rhi::ImageUsage::Storage,
                                    }, "IBL BRDF LUT");

        // ---- 4. Create sampler ----

        sampler_ = rm.create_sampler({
                                         .mag_filter = rhi::Filter::Linear,
                                         .min_filter = rhi::Filter::Linear,
                                         .mip_mode = rhi::SamplerMipMode::Linear,
                                         .wrap_u = rhi::SamplerWrapMode::ClampToEdge,
                                         .wrap_v = rhi::SamplerWrapMode::ClampToEdge,
                                         .max_anisotropy = 0.0f,
                                         .max_lod = VK_LOD_CLAMP_NONE,
                                     }, "IBL Sampler");

        // ---- 5. Compile compute shaders ----

        const auto equirect_spirv = sc.compile_from_file("ibl/equirect_to_cubemap.comp",
                                                         rhi::ShaderStage::Compute);
        const auto irradiance_spirv = sc.compile_from_file("ibl/irradiance.comp",
                                                           rhi::ShaderStage::Compute);
        const auto prefilter_spirv = sc.compile_from_file("ibl/prefilter.comp",
                                                          rhi::ShaderStage::Compute);
        const auto brdf_spirv = sc.compile_from_file("ibl/brdf_lut.comp",
                                                     rhi::ShaderStage::Compute);

        const auto equirect_module = rhi::create_shader_module(ctx.device, equirect_spirv);
        const auto irradiance_module = rhi::create_shader_module(ctx.device, irradiance_spirv);
        const auto prefilter_module = rhi::create_shader_module(ctx.device, prefilter_spirv);
        const auto brdf_module = rhi::create_shader_module(ctx.device, brdf_spirv);

        // ---- 6. Create push descriptor set layouts ----
        // Layout A: sampler + storage image (equirect→cubemap, irradiance, prefiltered)
        // Layout B: storage image only (BRDF LUT)

        constexpr VkDescriptorSetLayoutBinding bindings_a[]{
            {
                0,
                VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                1,
                VK_SHADER_STAGE_COMPUTE_BIT,
                nullptr
            },
            {
                1,
                VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                1,
                VK_SHADER_STAGE_COMPUTE_BIT,
                nullptr
            },
        };
        const auto layout_a = create_push_layout(ctx.device, bindings_a, 2);

        constexpr VkDescriptorSetLayoutBinding bindings_b[]{
            {
                0,
                VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                1,
                VK_SHADER_STAGE_COMPUTE_BIT,
                nullptr
            },
        };
        const auto layout_b = create_push_layout(ctx.device, bindings_b, 1);

        // ---- 7. Create compute pipelines ----

        auto equirect_pipeline = rhi::create_compute_pipeline(ctx.device, {
                                                                  .compute_shader = equirect_module,
                                                                  .descriptor_set_layouts = {layout_a},
                                                              });

        auto irradiance_pipeline = rhi::create_compute_pipeline(ctx.device, {
                                                                    .compute_shader = irradiance_module,
                                                                    .descriptor_set_layouts = {layout_a},
                                                                });

        auto prefilter_pipeline = rhi::create_compute_pipeline(ctx.device, {
                                                                   .compute_shader = prefilter_module,
                                                                   .descriptor_set_layouts = {layout_a},
                                                                   .push_constant_ranges = {
                                                                       {
                                                                           VK_SHADER_STAGE_COMPUTE_BIT,
                                                                           0,
                                                                           sizeof(float)
                                                                       }
                                                                   },
                                                               });

        auto brdf_pipeline = rhi::create_compute_pipeline(ctx.device, {
                                                              .compute_shader = brdf_module,
                                                              .descriptor_set_layouts = {layout_b},
                                                          });

        // ---- 8. Create temporary image views for storage writes ----
        // Cubemap images need VK_IMAGE_VIEW_TYPE_2D_ARRAY for image2DArray in shaders.

        const auto cubemap_vk = rm.get_image(cubemap_);
        const auto irradiance_vk = rm.get_image(irradiance_);
        const auto prefiltered_vk = rm.get_image(prefiltered_);
        const auto brdf_vk = rm.get_image(brdf_lut_);

        const auto cubemap_storage_view = create_storage_view(
            ctx.device, cubemap_vk.image,
            rhi::to_vk_format(rhi::Format::R16G16B16A16Sfloat), 0, 6);

        const auto irradiance_storage_view = create_storage_view(
            ctx.device, irradiance_vk.image,
            rhi::to_vk_format(rhi::Format::B10G11R11UfloatPack32), 0, 6);

        std::vector<VkImageView> prefiltered_mip_views(prefiltered_mip_count_);
        for (uint32_t mip = 0; mip < prefiltered_mip_count_; ++mip) {
            prefiltered_mip_views[mip] = create_storage_view(
                ctx.device, prefiltered_vk.image,
                rhi::to_vk_format(rhi::Format::R16G16B16A16Sfloat), mip, 6);
        }

        // Sampler info reused across dispatches
        const auto sampler_vk = rm.get_sampler(sampler_);

        // ---- 9. Execute compute dispatches in a single immediate scope ----

        ctx.begin_immediate();
        const auto raw_cmd = ctx.immediate_command_buffer;
        rhi::CommandBuffer cmd(raw_cmd);

        // 9a. Upload equirect (transitions: UNDEFINED → TRANSFER_DST → SHADER_READ_ONLY)
        rm.upload_image(equirect, rgba16.data(), rgba16.size() * sizeof(uint16_t));
        // Release CPU-side pixel data (no longer needed after upload recording)
        rgba16.clear();
        rgba16.shrink_to_fit();

        // upload_image targets FRAGMENT_SHADER stage; extend visibility to COMPUTE_SHADER
        const auto equirect_vk = rm.get_image(equirect);
        image_barrier(raw_cmd, equirect_vk.image,
                      VK_PIPELINE_STAGE_2_COPY_BIT,
                      VK_ACCESS_2_TRANSFER_WRITE_BIT,
                      VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                      VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                      1,
                      1);

        // 9b. Cubemap: UNDEFINED → GENERAL (prepare for storage write)
        image_barrier(raw_cmd, cubemap_vk.image,
                      VK_PIPELINE_STAGE_2_NONE,
                      VK_ACCESS_2_NONE,
                      VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                      VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                      VK_IMAGE_LAYOUT_UNDEFINED,
                      VK_IMAGE_LAYOUT_GENERAL,
                      1,
                      6);

        // 9c. Dispatch: equirect → cubemap
        {
            VkDescriptorImageInfo input_info{
                sampler_vk.sampler, equirect_vk.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
            };
            VkDescriptorImageInfo output_info{
                VK_NULL_HANDLE, cubemap_storage_view, VK_IMAGE_LAYOUT_GENERAL
            };
            const VkWriteDescriptorSet writes[]{
                push_write(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &input_info),
                push_write(1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &output_info),
            };
            cmd.bind_compute_pipeline(equirect_pipeline);
            cmd.push_descriptor_set(equirect_pipeline.layout, 0, writes);
            cmd.dispatch(kCubemapSize / kLocalSize, kCubemapSize / kLocalSize, 6);
        }

        // 9d. Cubemap: GENERAL → SHADER_READ_ONLY (prepare for sampling in irradiance/prefiltered)
        image_barrier(raw_cmd, cubemap_vk.image,
                      VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                      VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                      VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                      VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                      VK_IMAGE_LAYOUT_GENERAL,
                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                      1,
                      6);

        // 9e. Irradiance: UNDEFINED → GENERAL
        image_barrier(raw_cmd, irradiance_vk.image,
                      VK_PIPELINE_STAGE_2_NONE,
                      VK_ACCESS_2_NONE,
                      VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                      VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                      VK_IMAGE_LAYOUT_UNDEFINED,
                      VK_IMAGE_LAYOUT_GENERAL,
                      1,
                      6);

        // 9f. Dispatch: irradiance convolution
        {
            VkDescriptorImageInfo input_info{
                sampler_vk.sampler, cubemap_vk.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
            };
            VkDescriptorImageInfo output_info{
                VK_NULL_HANDLE, irradiance_storage_view, VK_IMAGE_LAYOUT_GENERAL
            };
            const VkWriteDescriptorSet writes[]{
                push_write(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &input_info),
                push_write(1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &output_info),
            };
            cmd.bind_compute_pipeline(irradiance_pipeline);
            cmd.push_descriptor_set(irradiance_pipeline.layout, 0, writes);
            cmd.dispatch(kIrradianceSize / kLocalSize,
                         kIrradianceSize / kLocalSize,
                         6);
        }

        // 9g. Irradiance: GENERAL → SHADER_READ_ONLY (final state for rendering)
        image_barrier(raw_cmd, irradiance_vk.image,
                      VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                      VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                      VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                      VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                      VK_IMAGE_LAYOUT_GENERAL,
                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                      1,
                      6);

        // 9h. Prefiltered: UNDEFINED → GENERAL (all mip levels)
        image_barrier(raw_cmd, prefiltered_vk.image,
                      VK_PIPELINE_STAGE_2_NONE,
                      VK_ACCESS_2_NONE,
                      VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                      VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                      VK_IMAGE_LAYOUT_UNDEFINED,
                      VK_IMAGE_LAYOUT_GENERAL,
                      prefiltered_mip_count_,
                      6);

        // 9i. Dispatch: prefiltered environment map (one dispatch per mip level)
        cmd.bind_compute_pipeline(prefilter_pipeline);
        for (uint32_t mip = 0; mip < prefiltered_mip_count_; ++mip) {
            const auto mip_size = std::max(kPrefilteredSize >> mip, 1u);
            const float roughness = static_cast<float>(mip) / static_cast<float>(prefiltered_mip_count_ - 1);

            VkDescriptorImageInfo input_info{
                sampler_vk.sampler, cubemap_vk.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
            };
            VkDescriptorImageInfo output_info{
                VK_NULL_HANDLE, prefiltered_mip_views[mip], VK_IMAGE_LAYOUT_GENERAL
            };
            const VkWriteDescriptorSet writes[]{
                push_write(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &input_info),
                push_write(1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &output_info),
            };
            cmd.push_descriptor_set(prefilter_pipeline.layout, 0, writes);
            cmd.push_constants(prefilter_pipeline.layout, VK_SHADER_STAGE_COMPUTE_BIT,
                               &roughness, sizeof(float));
            cmd.dispatch(
                std::max(mip_size / kLocalSize, 1u),
                std::max(mip_size / kLocalSize, 1u),
                6);
        }

        // 9j. Prefiltered: GENERAL → SHADER_READ_ONLY (final state for rendering)
        image_barrier(raw_cmd, prefiltered_vk.image,
                      VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                      VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                      VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                      VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                      VK_IMAGE_LAYOUT_GENERAL,
                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                      prefiltered_mip_count_,
                      6);

        // 9k. BRDF LUT: UNDEFINED → GENERAL
        image_barrier(raw_cmd, brdf_vk.image,
                      VK_PIPELINE_STAGE_2_NONE,
                      VK_ACCESS_2_NONE,
                      VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                      VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                      VK_IMAGE_LAYOUT_UNDEFINED,
                      VK_IMAGE_LAYOUT_GENERAL,
                      1,
                      1);

        // 9l. Dispatch: BRDF integration LUT
        {
            VkDescriptorImageInfo output_info{
                VK_NULL_HANDLE, brdf_vk.view, VK_IMAGE_LAYOUT_GENERAL
            };
            const VkWriteDescriptorSet writes[]{
                push_write(0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &output_info),
            };
            cmd.bind_compute_pipeline(brdf_pipeline);
            cmd.push_descriptor_set(brdf_pipeline.layout, 0, writes);
            cmd.dispatch(kBrdfLutSize / kLocalSize, kBrdfLutSize / kLocalSize, 1);
        }

        // 9m. BRDF LUT: GENERAL → SHADER_READ_ONLY (final state for rendering)
        image_barrier(raw_cmd, brdf_vk.image,
                      VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                      VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                      VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                      VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                      VK_IMAGE_LAYOUT_GENERAL,
                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                      1,
                      1);

        ctx.end_immediate();

        // ---- 10. Cleanup temporary resources ----

        // Equirect input image (no longer needed after cubemap conversion)
        rm.destroy_image(equirect);

        // Temporary storage views
        vkDestroyImageView(ctx.device, cubemap_storage_view, nullptr);
        vkDestroyImageView(ctx.device, irradiance_storage_view, nullptr);
        for (const auto view: prefiltered_mip_views) {
            vkDestroyImageView(ctx.device, view, nullptr);
        }

        // Compute pipelines (one-time use)
        equirect_pipeline.destroy(ctx.device);
        irradiance_pipeline.destroy(ctx.device);
        prefilter_pipeline.destroy(ctx.device);
        brdf_pipeline.destroy(ctx.device);

        // Shader modules
        vkDestroyShaderModule(ctx.device, equirect_module, nullptr);
        vkDestroyShaderModule(ctx.device, irradiance_module, nullptr);
        vkDestroyShaderModule(ctx.device, prefilter_module, nullptr);
        vkDestroyShaderModule(ctx.device, brdf_module, nullptr);

        // Push descriptor layouts
        vkDestroyDescriptorSetLayout(ctx.device, layout_a, nullptr);
        vkDestroyDescriptorSetLayout(ctx.device, layout_b, nullptr);

        // ---- 11. Register IBL products to bindless arrays ----

        cubemap_bindless_ = dm.register_cubemap(cubemap_, sampler_);
        irradiance_bindless_ = dm.register_cubemap(irradiance_, sampler_);
        prefiltered_bindless_ = dm.register_cubemap(prefiltered_, sampler_);
        brdf_lut_bindless_ = dm.register_texture(brdf_lut_, sampler_);

        spdlog::info("IBL: precomputation complete "
                     "(cubemap {}x{}, irradiance {}x{}, prefiltered {}x{} {}mip, BRDF LUT {}x{})",
                     kCubemapSize, kCubemapSize,
                     kIrradianceSize, kIrradianceSize,
                     kPrefilteredSize, kPrefilteredSize, prefiltered_mip_count_,
                     kBrdfLutSize, kBrdfLutSize);
    }

    // ---- IBL::destroy() ----

    void IBL::destroy() const {
        // Unregister bindless entries before destroying images
        if (cubemap_bindless_.valid())
            dm_->unregister_cubemap(cubemap_bindless_);
        if (irradiance_bindless_.valid())
            dm_->unregister_cubemap(irradiance_bindless_);
        if (prefiltered_bindless_.valid())
            dm_->unregister_cubemap(prefiltered_bindless_);
        if (brdf_lut_bindless_.valid())
            dm_->unregister_texture(brdf_lut_bindless_);

        // Destroy images
        if (cubemap_.valid())
            rm_->destroy_image(cubemap_);
        if (irradiance_.valid())
            rm_->destroy_image(irradiance_);
        if (prefiltered_.valid())
            rm_->destroy_image(prefiltered_);
        if (brdf_lut_.valid())
            rm_->destroy_image(brdf_lut_);

        // Destroy sampler
        if (sampler_.valid())
            rm_->destroy_sampler(sampler_);
    }

    // ---- Accessors ----

    rhi::BindlessIndex IBL::irradiance_cubemap_index() const {
        return irradiance_bindless_;
    }

    rhi::BindlessIndex IBL::prefiltered_cubemap_index() const {
        return prefiltered_bindless_;
    }

    rhi::BindlessIndex IBL::brdf_lut_index() const {
        return brdf_lut_bindless_;
    }

    rhi::BindlessIndex IBL::skybox_cubemap_index() const {
        return cubemap_bindless_;
    }

    uint32_t IBL::prefiltered_mip_count() const {
        return prefiltered_mip_count_;
    }
} // namespace himalaya::framework
