/**
 * @file forward_pass.cpp
 * @brief ForwardPass implementation — pipeline creation and per-frame recording.
 */

#include <himalaya/passes/forward_pass.h>

#include <himalaya/framework/frame_context.h>
#include <himalaya/framework/mesh.h>
#include <himalaya/framework/render_graph.h>
#include <himalaya/framework/scene_data.h>
#include <himalaya/rhi/commands.h>
#include <himalaya/rhi/context.h>
#include <himalaya/rhi/descriptors.h>
#include <himalaya/rhi/resources.h>
#include <himalaya/rhi/shader.h>

#include <array>

namespace himalaya::passes {
    // ---- HDR color attachment format (hardcoded per design) ----
    constexpr VkFormat kHdrColorFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
    constexpr VkFormat kDepthFormat = VK_FORMAT_D32_SFLOAT;

    // ---- Init / Destroy ----

    void ForwardPass::setup(rhi::Context &ctx,
                            rhi::ResourceManager &rm,
                            rhi::DescriptorManager &dm,
                            rhi::ShaderCompiler &sc,
                            const uint32_t sample_count) {
        ctx_ = &ctx;
        rm_ = &rm;
        dm_ = &dm;
        sc_ = &sc;

        current_sample_count_ = sample_count;
        create_pipelines(sample_count);
    }

    void ForwardPass::on_sample_count_changed(const uint32_t sample_count) {
        current_sample_count_ = sample_count;
        create_pipelines(sample_count);
    }

    void ForwardPass::rebuild_pipelines() {
        create_pipelines(current_sample_count_);
    }

    void ForwardPass::destroy() const {
        pipeline_.destroy(ctx_->device);
    }

    // ---- Pipeline creation ----

    void ForwardPass::create_pipelines(const uint32_t sample_count) {
        // Destroy previous pipeline if rebuilding (MSAA switch).
        if (pipeline_.pipeline != VK_NULL_HANDLE) {
            pipeline_.destroy(ctx_->device);
        }

        const auto vert_spirv = sc_->compile_from_file("forward.vert",
                                                       rhi::ShaderStage::Vertex);
        const auto frag_spirv = sc_->compile_from_file("forward.frag",
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
        desc.sample_count = sample_count;

        const auto binding = framework::Vertex::binding_description();
        const auto attributes = framework::Vertex::attribute_descriptions();
        desc.vertex_bindings = {binding};
        desc.vertex_attributes = {attributes.begin(), attributes.end()};

        const auto set_layouts = dm_->get_global_set_layouts();
        desc.descriptor_set_layouts = {set_layouts.begin(), set_layouts.end()};

        pipeline_ = rhi::create_graphics_pipeline(ctx_->device, desc);

        vkDestroyShaderModule(ctx_->device, frag_module, nullptr);
        vkDestroyShaderModule(ctx_->device, vert_module, nullptr);
    }

    // ---- Per-frame recording ----

    void ForwardPass::record(framework::RenderGraph &rg, const framework::FrameContext &ctx) const {
        const bool msaa = ctx.sample_count > 1;

        // Execute lambda is shared between MSAA and 1x paths — only
        // attachment setup differs, the draw loop is identical.
        auto execute = [this, &rg, &ctx, msaa](const rhi::CommandBuffer &cmd) {
            // Color attachment: render to MSAA target with resolve, or directly to hdr_color
            const auto color_target = rg.get_image(msaa ? ctx.msaa_color : ctx.hdr_color);

            VkRenderingAttachmentInfo color_attachment{};
            color_attachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            color_attachment.imageView = rm_->get_image(color_target).view;
            color_attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            color_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            color_attachment.storeOp = msaa
                                           ? VK_ATTACHMENT_STORE_OP_DONT_CARE
                                           : VK_ATTACHMENT_STORE_OP_STORE;
            color_attachment.clearValue.color = {{0.0f, 0.0f, 0.0f, 1.0f}};

            if (msaa) {
                const auto resolve_target = rg.get_image(ctx.hdr_color);
                color_attachment.resolveMode = VK_RESOLVE_MODE_AVERAGE_BIT;
                color_attachment.resolveImageView = rm_->get_image(resolve_target).view;
                color_attachment.resolveImageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            }

            // Depth attachment: read-only (EQUAL test, no write, no resolve).
            // PrePass already filled the depth buffer; Forward only tests against it.
            const auto depth_target = rg.get_image(msaa ? ctx.msaa_depth : ctx.depth);

            VkRenderingAttachmentInfo depth_attachment{};
            depth_attachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            depth_attachment.imageView = rm_->get_image(depth_target).view;
            depth_attachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL;
            depth_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
            depth_attachment.storeOp = VK_ATTACHMENT_STORE_OP_NONE;

            // Derive render area from the color render target
            const auto &color_image = rm_->get_image(color_target);
            const VkExtent2D render_extent{color_image.desc.width, color_image.desc.height};

            VkRenderingInfo rendering_info{};
            rendering_info.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
            rendering_info.renderArea = {{0, 0}, render_extent};
            rendering_info.layerCount = 1;
            rendering_info.colorAttachmentCount = 1;
            rendering_info.pColorAttachments = &color_attachment;
            rendering_info.pDepthAttachment = &depth_attachment;

            cmd.begin_rendering(rendering_info);
            cmd.bind_pipeline(pipeline_);

            const VkDescriptorSet sets[] = {
                dm_->get_set0(ctx.frame_index),
                dm_->get_set1(),
            };
            cmd.bind_descriptor_sets(pipeline_.layout, 0, sets, 2);

            VkViewport viewport{};
            viewport.x = 0.0f;
            viewport.y = static_cast<float>(render_extent.height);
            viewport.width = static_cast<float>(render_extent.width);
            viewport.height = -static_cast<float>(render_extent.height);
            viewport.minDepth = 0.0f;
            viewport.maxDepth = 1.0f;
            cmd.set_viewport(viewport);
            cmd.set_scissor({{0, 0}, render_extent});

            cmd.set_front_face(VK_FRONT_FACE_COUNTER_CLOCKWISE);
            cmd.set_depth_test_enable(true);
            cmd.set_depth_write_enable(false);
            cmd.set_depth_compare_op(VK_COMPARE_OP_EQUAL);

            // Draw visible opaque instances via instanced draw groups.
            // Transparent instances are skipped — EQUAL test rejects them since
            // they were not rendered in PrePass. Phase 7 Transparent Pass fixes this.
            auto draw_group = [&](const framework::MeshDrawGroup &group) {
                const auto &mesh = ctx.meshes[group.mesh_id];

                cmd.set_cull_mode(
                    group.double_sided
                        ? VK_CULL_MODE_NONE
                        : VK_CULL_MODE_BACK_BIT);

                cmd.bind_vertex_buffer(
                    0,
                    rm_->get_buffer(mesh.vertex_buffer).buffer);
                cmd.bind_index_buffer(
                    rm_->get_buffer(mesh.index_buffer).buffer,
                    VK_INDEX_TYPE_UINT32);
                cmd.draw_indexed(mesh.index_count, group.instance_count, 0, 0, group.first_instance);
            };

            for (const auto &group : ctx.opaque_draw_groups)
                draw_group(group);
            for (const auto &group : ctx.mask_draw_groups)
                draw_group(group);

            cmd.end_rendering();
        };

        // Resource declarations differ by MSAA mode:
        // MSAA: msaa_color(W) + msaa_depth(R) + hdr_color(W, resolve target)
        // 1x:   hdr_color(W) + depth(R)
        // Depth is read-only: EQUAL test, no write, no resolve (PrePass owns depth).
        if (msaa) {
            const std::array resources = {
                framework::RGResourceUsage{
                    ctx.msaa_color,
                    framework::RGAccessType::Write,
                    framework::RGStage::ColorAttachment,
                },
                framework::RGResourceUsage{
                    ctx.msaa_depth,
                    framework::RGAccessType::Read,
                    framework::RGStage::DepthAttachment,
                },
                framework::RGResourceUsage{
                    ctx.hdr_color,
                    framework::RGAccessType::Write,
                    framework::RGStage::ColorAttachment,
                },
            };
            rg.add_pass("Forward", resources, execute);
        } else {
            const std::array resources = {
                framework::RGResourceUsage{
                    ctx.hdr_color,
                    framework::RGAccessType::Write,
                    framework::RGStage::ColorAttachment,
                },
                framework::RGResourceUsage{
                    ctx.depth,
                    framework::RGAccessType::Read,
                    framework::RGStage::DepthAttachment,
                },
            };
            rg.add_pass("Forward", resources, execute);
        }
    }
} // namespace himalaya::passes
