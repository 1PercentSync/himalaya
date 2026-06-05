/**
 * @file gaussian_splat_draw_pass.cpp
 * @brief GaussianSplatDrawPass implementation.
 */

#include <himalaya/passes/gaussian_splat_draw_pass.h>

#include <himalaya/framework/gaussian_splat_data.h>
#include <himalaya/framework/render_graph.h>
#include <himalaya/rhi/commands.h>
#include <himalaya/rhi/context.h>
#include <himalaya/rhi/descriptors.h>
#include <himalaya/rhi/resources.h>
#include <himalaya/rhi/shader.h>

#include <array>
#include <utility>
#include <vector>

#include <spdlog/spdlog.h>

namespace himalaya::passes {
    void GaussianSplatDrawPass::setup(rhi::Context &ctx,
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

    void GaussianSplatDrawPass::record(framework::RenderGraph &rg,
                                       const framework::RGResourceId composition_target,
                                       const GaussianSplatGraphResources &resources,
                                       const framework::GaussianSplatGpuScene &scene,
                                       const uint32_t frame_index,
                                       const GSPushConstants &push_constants) const {
        if (pipeline_.pipeline == VK_NULL_HANDLE || !rm_ || scene.total_splat_count == 0u ||
            scene.descriptor_set == VK_NULL_HANDLE) {
            return;
        }

        const std::array usages = {
            framework::RGResourceUsage{
                composition_target,
                framework::RGAccessType::Write,
                framework::RGStage::ColorAttachment,
            },
            framework::RGResourceUsage{
                resources.projected_data,
                framework::RGAccessType::Read,
                framework::RGStage::Vertex,
            },
            framework::RGResourceUsage{
                resources.sort_entries,
                framework::RGAccessType::Read,
                framework::RGStage::Vertex,
            },
            framework::RGResourceUsage{
                resources.indirect_draw,
                framework::RGAccessType::Read,
                framework::RGStage::DrawIndirect,
            },
        };

        rg.add_pass("GS Draw", usages,
                    [this, &rg, composition_target, resources, scene, frame_index, push_constants](
                        const rhi::CommandBuffer &cmd) {
                        const auto composition_handle = rg.get_image(composition_target);
                        const auto &composition_image = rm_->get_image(composition_handle);
                        const VkExtent2D render_extent{
                            composition_image.desc.width,
                            composition_image.desc.height,
                        };

                        VkRenderingAttachmentInfo color_attachment{};
                        color_attachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
                        color_attachment.imageView = composition_image.view;
                        color_attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                        color_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
                        color_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
                        color_attachment.clearValue.color = {{0.0f, 0.0f, 0.0f, 0.0f}};

                        VkRenderingInfo rendering_info{};
                        rendering_info.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
                        rendering_info.renderArea = {{0, 0}, render_extent};
                        rendering_info.layerCount = 1;
                        rendering_info.colorAttachmentCount = 1;
                        rendering_info.pColorAttachments = &color_attachment;

                        cmd.begin_rendering(rendering_info);
                        cmd.bind_pipeline(pipeline_);

                        const std::array sets = {
                            dm_->get_set0(frame_index),
                            dm_->get_set1(),
                            dm_->get_set2(frame_index),
                            scene.descriptor_set,
                        };
                        cmd.bind_descriptor_sets(pipeline_.layout,
                                                 0,
                                                 sets.data(),
                                                 static_cast<uint32_t>(sets.size()));

                        cmd.push_constants(pipeline_.layout,
                                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                                           &push_constants,
                                           sizeof(push_constants));

                        VkViewport viewport{};
                        viewport.x = 0.0f;
                        viewport.y = 0.0f;
                        viewport.width = static_cast<float>(render_extent.width);
                        viewport.height = static_cast<float>(render_extent.height);
                        viewport.minDepth = 0.0f;
                        viewport.maxDepth = 1.0f;
                        cmd.set_viewport(viewport);
                        cmd.set_scissor({{0, 0}, render_extent});

                        cmd.set_cull_mode(VK_CULL_MODE_NONE);
                        cmd.set_front_face(VK_FRONT_FACE_COUNTER_CLOCKWISE);
                        cmd.set_depth_test_enable(false);
                        cmd.set_depth_write_enable(false);

                        const auto indirect_handle = rg.get_buffer(resources.indirect_draw);
                        const auto &indirect_buffer = rm_->get_buffer(indirect_handle);
                        cmd.draw_indirect(indirect_buffer.buffer,
                                          0,
                                          1,
                                          sizeof(framework::GaussianSplatDrawIndirectCommand));

                        cmd.end_rendering();
                    });
    }

    void GaussianSplatDrawPass::rebuild_pipelines() {
        create_pipeline();
    }

    void GaussianSplatDrawPass::destroy() {
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

    void GaussianSplatDrawPass::create_pipeline() {
        if (!ctx_ || !dm_ || !sc_ || gs_set3_layout_ == VK_NULL_HANDLE) {
            return;
        }

        const auto vert_spirv = sc_->compile_from_file("gs/draw.vert", rhi::ShaderStage::Vertex);
        const auto frag_spirv = sc_->compile_from_file("gs/draw.frag", rhi::ShaderStage::Fragment);
        if (vert_spirv.empty() || frag_spirv.empty()) {
            spdlog::warn("GaussianSplatDrawPass: shader compilation failed, keeping previous pipeline");
            return;
        }

        if (pipeline_.pipeline != VK_NULL_HANDLE) {
            pipeline_.destroy(ctx_->device);
            pipeline_ = {};
        }

        const VkShaderModule vert_module = rhi::create_shader_module(ctx_->device, vert_spirv);
        const VkShaderModule frag_module = rhi::create_shader_module(ctx_->device, frag_spirv);

        auto global_set_layouts = dm_->get_graphics_set_layouts();
        std::vector<VkDescriptorSetLayout> set_layouts{global_set_layouts.begin(),
                                                       global_set_layouts.end()};
        set_layouts.push_back(gs_set3_layout_);

        const VkPushConstantRange push_range{
            .stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
            .offset = 0,
            .size = sizeof(GSPushConstants),
        };

        rhi::GraphicsPipelineDesc desc{};
        desc.vertex_shader = vert_module;
        desc.fragment_shader = frag_module;
        desc.color_formats = {kGaussianSplatCompositionFormat};
        desc.depth_format = VK_FORMAT_UNDEFINED;
        desc.sample_count = 1;
        desc.blend_mode = rhi::BlendMode::PremultipliedUnder;
        desc.descriptor_set_layouts = std::move(set_layouts);
        desc.push_constant_ranges = {push_range};

        pipeline_ = rhi::create_graphics_pipeline(ctx_->device, desc);

        vkDestroyShaderModule(ctx_->device, frag_module, nullptr);
        vkDestroyShaderModule(ctx_->device, vert_module, nullptr);
    }
} // namespace himalaya::passes
