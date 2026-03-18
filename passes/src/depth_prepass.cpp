/**
 * @file depth_prepass.cpp
 * @brief DepthPrePass implementation — pipeline creation and per-frame recording.
 *
 * Two pipelines share the same vertex shader (depth_prepass.vert with
 * invariant gl_Position). Opaque pipeline uses depth_prepass.frag (no
 * discard), Mask pipeline uses depth_prepass_masked.frag (alpha test).
 *
 * When MSAA is active, Dynamic Rendering resolves depth (MAX_BIT) and
 * normal (AVERAGE) to 1x resolved targets in the same render pass.
 */

#include <himalaya/passes/depth_prepass.h>

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
    // ---- Attachment formats (hardcoded per design) ----
    constexpr VkFormat kDepthFormat = VK_FORMAT_D32_SFLOAT;
    constexpr VkFormat kNormalFormat = VK_FORMAT_A2B10G10R10_UNORM_PACK32;

    // ---- Init / Destroy ----

    void DepthPrePass::setup(rhi::Context &ctx,
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

    // ReSharper disable once CppMemberFunctionMayBeStatic
    void DepthPrePass::on_resize([[maybe_unused]] const uint32_t width,
                                 [[maybe_unused]] const uint32_t height) {
        // No resolution-dependent private resources.
    }

    void DepthPrePass::on_sample_count_changed(const uint32_t sample_count) {
        current_sample_count_ = sample_count;
        create_pipelines(sample_count);
    }

    void DepthPrePass::rebuild_pipelines() {
        create_pipelines(current_sample_count_);
    }

    void DepthPrePass::destroy() const {
        opaque_pipeline_.destroy(ctx_->device);
        mask_pipeline_.destroy(ctx_->device);
    }

    // ---- Pipeline creation ----

    void DepthPrePass::create_pipelines(const uint32_t sample_count) {
        // Destroy previous pipelines if rebuilding (MSAA switch).
        if (opaque_pipeline_.pipeline != VK_NULL_HANDLE)
            opaque_pipeline_.destroy(ctx_->device);
        if (mask_pipeline_.pipeline != VK_NULL_HANDLE)
            mask_pipeline_.destroy(ctx_->device);

        // Shared vertex shader (invariant gl_Position)
        const auto vert_spirv = sc_->compile_from_file("depth_prepass.vert",
                                                       rhi::ShaderStage::Vertex);
        // ReSharper disable once CppLocalVariableMayBeConst
        VkShaderModule vert_module = rhi::create_shader_module(ctx_->device, vert_spirv);

        // Shared pipeline descriptor
        const auto binding = framework::Vertex::binding_description();
        const auto attributes = framework::Vertex::attribute_descriptions();
        const auto set_layouts = dm_->get_global_set_layouts();

        auto make_desc = [&](const VkShaderModule frag_module) {
            rhi::GraphicsPipelineDesc desc;
            desc.vertex_shader = vert_module;
            desc.fragment_shader = frag_module;
            desc.color_formats = {kNormalFormat};
            desc.depth_format = kDepthFormat;
            desc.sample_count = sample_count;
            desc.vertex_bindings = {binding};
            desc.vertex_attributes = {attributes.begin(), attributes.end()};
            desc.descriptor_set_layouts = {set_layouts.begin(), set_layouts.end()};
            return desc;
        };

        // Opaque pipeline (no discard)
        {
            const auto frag_spirv = sc_->compile_from_file("depth_prepass.frag",
                                                           rhi::ShaderStage::Fragment);
            // ReSharper disable once CppLocalVariableMayBeConst
            VkShaderModule frag_module = rhi::create_shader_module(ctx_->device, frag_spirv);

            opaque_pipeline_ = rhi::create_graphics_pipeline(ctx_->device, make_desc(frag_module));
            vkDestroyShaderModule(ctx_->device, frag_module, nullptr);
        }

        // Mask pipeline (alpha test + discard)
        {
            const auto frag_spirv = sc_->compile_from_file("depth_prepass_masked.frag",
                                                           rhi::ShaderStage::Fragment);
            // ReSharper disable once CppLocalVariableMayBeConst
            VkShaderModule frag_module = rhi::create_shader_module(ctx_->device, frag_spirv);

            mask_pipeline_ = rhi::create_graphics_pipeline(ctx_->device, make_desc(frag_module));
            vkDestroyShaderModule(ctx_->device, frag_module, nullptr);
        }

        vkDestroyShaderModule(ctx_->device, vert_module, nullptr);
    }

    // ---- Per-frame recording ----

    void DepthPrePass::record(framework::RenderGraph &rg,
                              const framework::FrameContext &ctx) const {
        const bool msaa = ctx.sample_count > 1;

        auto execute = [this, &rg, &ctx, msaa](const rhi::CommandBuffer &cmd) {
            // Normal attachment: render to MSAA normal with resolve, or directly to 1x normal
            const auto normal_target = rg.get_image(msaa ? ctx.msaa_normal : ctx.normal);

            VkRenderingAttachmentInfo normal_attachment{};
            normal_attachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            normal_attachment.imageView = rm_->get_image(normal_target).view;
            normal_attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            normal_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            normal_attachment.storeOp = msaa
                                            ? VK_ATTACHMENT_STORE_OP_DONT_CARE
                                            : VK_ATTACHMENT_STORE_OP_STORE;
            normal_attachment.clearValue.color = {{0.0f, 0.0f, 0.0f, 0.0f}};

            if (msaa) {
                const auto resolve_target = rg.get_image(ctx.normal);
                normal_attachment.resolveMode = VK_RESOLVE_MODE_AVERAGE_BIT;
                normal_attachment.resolveImageView = rm_->get_image(resolve_target).view;
                normal_attachment.resolveImageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            }

            // Depth attachment: MSAA depth with resolve, or 1x depth directly
            const auto depth_target = rg.get_image(msaa ? ctx.msaa_depth : ctx.depth);

            VkRenderingAttachmentInfo depth_attachment{};
            depth_attachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            depth_attachment.imageView = rm_->get_image(depth_target).view;
            depth_attachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
            depth_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            depth_attachment.storeOp = msaa
                                           ? VK_ATTACHMENT_STORE_OP_DONT_CARE
                                           : VK_ATTACHMENT_STORE_OP_STORE;
            depth_attachment.clearValue.depthStencil = {0.0f, 0}; // Reverse-Z: far = 0

            if (msaa) {
                const auto resolve_target = rg.get_image(ctx.depth);
                depth_attachment.resolveMode = VK_RESOLVE_MODE_MAX_BIT;
                depth_attachment.resolveImageView = rm_->get_image(resolve_target).view;
                depth_attachment.resolveImageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
            }

            // Derive render area from depth target
            const auto &depth_image = rm_->get_image(depth_target);
            const VkExtent2D render_extent{depth_image.desc.width, depth_image.desc.height};

            VkRenderingInfo rendering_info{};
            rendering_info.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
            rendering_info.renderArea = {{0, 0}, render_extent};
            rendering_info.layerCount = 1;
            rendering_info.colorAttachmentCount = 1;
            rendering_info.pColorAttachments = &normal_attachment;
            rendering_info.pDepthAttachment = &depth_attachment;

            cmd.begin_rendering(rendering_info);

            // Shared dynamic state
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
            cmd.set_depth_write_enable(true);
            cmd.set_depth_compare_op(VK_COMPARE_OP_GREATER); // Reverse-Z

            // Draw helper: bind buffers, instanced draw
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

            // Bind common descriptor sets
            const VkDescriptorSet sets[] = {
                dm_->get_set0(ctx.frame_index),
                dm_->get_set1(),
            };

            // --- Opaque batch first (Early-Z guaranteed) ---
            cmd.bind_pipeline(opaque_pipeline_);
            cmd.bind_descriptor_sets(opaque_pipeline_.layout, 0, sets, 2);

            for (const auto &group : ctx.opaque_draw_groups)
                draw_group(group);

            // --- Mask batch second (uses existing depth to reject occluded fragments) ---
            cmd.bind_pipeline(mask_pipeline_);
            cmd.bind_descriptor_sets(mask_pipeline_.layout, 0, sets, 2);

            for (const auto &group : ctx.mask_draw_groups)
                draw_group(group);

            cmd.end_rendering();
        };

        // Resource declarations differ by MSAA mode:
        // MSAA: msaa_depth(RW) + msaa_normal(W) + depth(W, resolve) + normal(W, resolve)
        // 1x:   depth(RW) + normal(W)
        if (msaa) {
            const std::array resources = {
                framework::RGResourceUsage{
                    ctx.msaa_depth,
                    framework::RGAccessType::ReadWrite,
                    framework::RGStage::DepthAttachment,
                },
                framework::RGResourceUsage{
                    ctx.msaa_normal,
                    framework::RGAccessType::Write,
                    framework::RGStage::ColorAttachment,
                },
                framework::RGResourceUsage{
                    ctx.depth,
                    framework::RGAccessType::Write,
                    framework::RGStage::DepthAttachment,
                },
                framework::RGResourceUsage{
                    ctx.normal,
                    framework::RGAccessType::Write,
                    framework::RGStage::ColorAttachment,
                },
            };
            rg.add_pass("DepthPrePass", resources, execute);
        } else {
            const std::array resources = {
                framework::RGResourceUsage{
                    ctx.depth,
                    framework::RGAccessType::ReadWrite,
                    framework::RGStage::DepthAttachment,
                },
                framework::RGResourceUsage{
                    ctx.normal,
                    framework::RGAccessType::Write,
                    framework::RGStage::ColorAttachment,
                },
            };
            rg.add_pass("DepthPrePass", resources, execute);
        }
    }
} // namespace himalaya::passes
