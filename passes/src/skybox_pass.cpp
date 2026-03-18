/**
 * @file skybox_pass.cpp
 * @brief SkyboxPass implementation — pipeline creation and per-frame recording.
 *
 * Renders the environment cubemap as sky background into resolved hdr_color.
 * Uses GREATER_OR_EQUAL depth test with reverse-Z: sky pixels (depth 0.0)
 * only pass where no geometry wrote depth (cleared to 0.0).
 */

#include <himalaya/passes/skybox_pass.h>

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

    void SkyboxPass::setup(rhi::Context &ctx,
                           rhi::ResourceManager &rm,
                           rhi::DescriptorManager &dm,
                           rhi::ShaderCompiler &sc) {
        ctx_ = &ctx;
        rm_ = &rm;
        dm_ = &dm;
        sc_ = &sc;

        create_pipelines();
    }

    void SkyboxPass::rebuild_pipelines() {
        create_pipelines();
    }

    void SkyboxPass::destroy() const {
        pipeline_.destroy(ctx_->device);
    }

    // ---- Pipeline creation ----

    void SkyboxPass::create_pipelines() {
        // Compile shaders first — if compilation fails, keep the old pipeline
        // intact so the renderer can continue with the previous working shaders.
        const auto vert_spirv = sc_->compile_from_file("skybox.vert",
                                                        rhi::ShaderStage::Vertex);
        const auto frag_spirv = sc_->compile_from_file("skybox.frag",
                                                        rhi::ShaderStage::Fragment);

        if (vert_spirv.empty() || frag_spirv.empty()) {
            spdlog::warn("SkyboxPass: shader compilation failed, keeping previous pipeline");
            return;
        }

        // Shaders compiled successfully — safe to destroy old pipeline
        if (pipeline_.pipeline != VK_NULL_HANDLE) {
            pipeline_.destroy(ctx_->device);
        }

        // ReSharper disable once CppLocalVariableMayBeConst
        VkShaderModule vert_module = rhi::create_shader_module(ctx_->device, vert_spirv);
        // ReSharper disable once CppLocalVariableMayBeConst
        VkShaderModule frag_module = rhi::create_shader_module(ctx_->device, frag_spirv);

        rhi::GraphicsPipelineDesc desc;
        desc.vertex_shader = vert_module;
        desc.fragment_shader = frag_module;
        desc.color_formats = {VK_FORMAT_R16G16B16A16_SFLOAT}; // hdr_color
        desc.depth_format = VK_FORMAT_D32_SFLOAT;
        desc.sample_count = 1; // Always 1x — renders to resolved targets
        // No vertex input (fullscreen triangle from gl_VertexIndex)
        // No push constants

        const auto set_layouts = dm_->get_global_set_layouts();
        desc.descriptor_set_layouts = {set_layouts.begin(), set_layouts.end()};

        pipeline_ = rhi::create_graphics_pipeline(ctx_->device, desc);

        vkDestroyShaderModule(ctx_->device, frag_module, nullptr);
        vkDestroyShaderModule(ctx_->device, vert_module, nullptr);
    }

    // ---- Per-frame recording ----

    void SkyboxPass::record(framework::RenderGraph &rg,
                            const framework::FrameContext &ctx) const {
        const std::array resources = {
            framework::RGResourceUsage{
                ctx.depth,
                framework::RGAccessType::Read,
                framework::RGStage::DepthAttachment,
            },
            framework::RGResourceUsage{
                ctx.hdr_color,
                framework::RGAccessType::ReadWrite,
                framework::RGStage::ColorAttachment,
            },
        };

        rg.add_pass("Skybox", resources,
                    [this, &rg, &ctx](const rhi::CommandBuffer &cmd) {
                        // Color attachment: load forward pass output, store with sky added
                        const auto color_target = rg.get_image(ctx.hdr_color);
                        VkRenderingAttachmentInfo color_attachment{};
                        color_attachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
                        color_attachment.imageView = rm_->get_image(color_target).view;
                        color_attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                        color_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
                        color_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

                        // Depth attachment: read-only (GREATER_OR_EQUAL test, no write)
                        const auto depth_target = rg.get_image(ctx.depth);
                        VkRenderingAttachmentInfo depth_attachment{};
                        depth_attachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
                        depth_attachment.imageView = rm_->get_image(depth_target).view;
                        depth_attachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL;
                        depth_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
                        depth_attachment.storeOp = VK_ATTACHMENT_STORE_OP_NONE;

                        const auto &color_image = rm_->get_image(color_target);
                        const VkExtent2D render_extent{
                            color_image.desc.width, color_image.desc.height
                        };

                        VkRenderingInfo rendering_info{};
                        rendering_info.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
                        rendering_info.renderArea = {{0, 0}, render_extent};
                        rendering_info.layerCount = 1;
                        rendering_info.colorAttachmentCount = 1;
                        rendering_info.pColorAttachments = &color_attachment;
                        rendering_info.pDepthAttachment = &depth_attachment;

                        cmd.begin_rendering(rendering_info);
                        cmd.bind_pipeline(pipeline_);

                        // Bind Set 0 (GlobalUBO for inv_view_projection, camera, skybox index)
                        // + Set 1 (cubemaps[] bindless array)
                        const VkDescriptorSet sets[] = {
                            dm_->get_set0(ctx.frame_index),
                            dm_->get_set1(),
                        };
                        cmd.bind_descriptor_sets(pipeline_.layout, 0, sets, 2);

                        // Y-flipped viewport: skybox computes world directions from NDC,
                        // must match the GLM projection convention (Y up in clip space).
                        VkViewport viewport{};
                        viewport.x = 0.0f;
                        viewport.y = static_cast<float>(render_extent.height);
                        viewport.width = static_cast<float>(render_extent.width);
                        viewport.height = -static_cast<float>(render_extent.height);
                        viewport.minDepth = 0.0f;
                        viewport.maxDepth = 1.0f;
                        cmd.set_viewport(viewport);
                        cmd.set_scissor({{0, 0}, render_extent});

                        cmd.set_cull_mode(VK_CULL_MODE_NONE);
                        cmd.set_depth_test_enable(true);
                        cmd.set_depth_write_enable(false);
                        cmd.set_depth_compare_op(VK_COMPARE_OP_GREATER_OR_EQUAL);

                        cmd.draw(3); // Fullscreen triangle

                        cmd.end_rendering();
                    });
    }
} // namespace himalaya::passes
