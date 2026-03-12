/**
 * @file skybox_pass.cpp
 * @brief SkyboxPass implementation — pipeline creation and per-frame recording.
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

namespace himalaya::passes {
    // ---- Attachment formats (hardcoded per design) ----
    constexpr VkFormat kHdrColorFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
    constexpr VkFormat kDepthFormat = VK_FORMAT_D32_SFLOAT;

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

    void SkyboxPass::destroy() const {
        pipeline_.destroy(ctx_->device);
    }

    // ---- Pipeline creation ----

    void SkyboxPass::create_pipelines() {
        if (pipeline_.pipeline != VK_NULL_HANDLE) {
            pipeline_.destroy(ctx_->device);
        }

        const auto vert_spirv = sc_->compile_from_file("skybox.vert",
                                                        rhi::ShaderStage::Vertex);
        const auto frag_spirv = sc_->compile_from_file("skybox.frag",
                                                        rhi::ShaderStage::Fragment);

        // ReSharper disable once CppLocalVariableMayBeConst
        VkShaderModule vert_module = rhi::create_shader_module(ctx_->device, vert_spirv);
        // ReSharper disable once CppLocalVariableMayBeConst
        VkShaderModule frag_module = rhi::create_shader_module(ctx_->device, frag_spirv);

        rhi::GraphicsPipelineDesc desc;
        desc.vertex_shader = vert_module;
        desc.fragment_shader = frag_module;
        desc.color_formats = {kHdrColorFormat};
        desc.depth_format = kDepthFormat;
        desc.sample_count = 1; // Always 1x — renders to resolved buffer
        // No vertex input (fullscreen triangle from gl_VertexIndex)

        const auto set_layouts = dm_->get_global_set_layouts();
        desc.descriptor_set_layouts = {set_layouts.begin(), set_layouts.end()};

        // No push constants needed for skybox

        pipeline_ = rhi::create_graphics_pipeline(ctx_->device, desc);

        vkDestroyShaderModule(ctx_->device, frag_module, nullptr);
        vkDestroyShaderModule(ctx_->device, vert_module, nullptr);
    }

    // ---- Per-frame recording ----

    void SkyboxPass::record(framework::RenderGraph &rg,
                            const framework::FrameContext &ctx) const {
        // hdr_color: ReadWrite (LOAD existing forward output, STORE sky pixels)
        // depth: Read (GREATER_OR_EQUAL test, no write)
        const std::array resources = {
            framework::RGResourceUsage{
                ctx.hdr_color,
                framework::RGAccessType::ReadWrite,
                framework::RGStage::ColorAttachment,
            },
            framework::RGResourceUsage{
                ctx.depth,
                framework::RGAccessType::Read,
                framework::RGStage::DepthAttachment,
            },
        };

        rg.add_pass("Skybox", resources,
                    [this, &rg, &ctx](const rhi::CommandBuffer &cmd) {
                        // Color attachment: load forward pass result, store sky pixels
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

                        // Bind Set 0 (GlobalUBO for inv_view_projection, camera_position,
                        // skybox_cubemap_index) + Set 1 (bindless cubemaps[]).
                        const VkDescriptorSet sets[] = {
                            dm_->get_set0(ctx.frame_index),
                            dm_->get_set1(),
                        };
                        cmd.bind_descriptor_sets(pipeline_.layout, 0, sets, 2);

                        // Flipped viewport: skybox.vert uses inv_view_projection built
                        // with GLM (OpenGL Y-up convention), needs Y-flip to match.
                        VkViewport viewport{};
                        viewport.x = 0.0f;
                        viewport.y = static_cast<float>(render_extent.height);
                        viewport.width = static_cast<float>(render_extent.width);
                        viewport.height = -static_cast<float>(render_extent.height);
                        viewport.minDepth = 0.0f;
                        viewport.maxDepth = 1.0f;
                        cmd.set_viewport(viewport);
                        cmd.set_scissor({{0, 0}, render_extent});

                        // Reverse-Z: sky depth = 0.0, geometry depth > 0.0.
                        // GREATER_OR_EQUAL passes only sky pixels (0.0 >= 0.0).
                        cmd.set_cull_mode(VK_CULL_MODE_NONE);
                        cmd.set_depth_test_enable(true);
                        cmd.set_depth_write_enable(false);
                        cmd.set_depth_compare_op(VK_COMPARE_OP_GREATER_OR_EQUAL);

                        cmd.draw(3); // Fullscreen triangle

                        cmd.end_rendering();
                    });
    }
} // namespace himalaya::passes
