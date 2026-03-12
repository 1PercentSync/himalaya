/**
 * @file forward_pass.cpp
 * @brief ForwardPass implementation — pipeline creation and per-frame recording.
 */

#include <himalaya/passes/forward_pass.h>

#include <himalaya/framework/frame_context.h>
#include <himalaya/framework/material_system.h>
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

        create_pipelines(sample_count);
    }

    void ForwardPass::on_resize([[maybe_unused]] const uint32_t width,
                                [[maybe_unused]] const uint32_t height) {
        // Step 4a: no resolution-dependent private resources.
    }

    void ForwardPass::on_sample_count_changed(const uint32_t sample_count) {
        create_pipelines(sample_count);
    }

    void ForwardPass::destroy() {
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

        desc.push_constant_ranges = {
            {
                .stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                .offset = 0,
                .size = sizeof(framework::PushConstantData),
            },
        };

        pipeline_ = rhi::create_graphics_pipeline(ctx_->device, desc);

        vkDestroyShaderModule(ctx_->device, frag_module, nullptr);
        vkDestroyShaderModule(ctx_->device, vert_module, nullptr);
    }

    // ---- Per-frame recording ----

    void ForwardPass::record(framework::RenderGraph &rg, const framework::FrameContext &ctx) {
        // Declare resource usage: write hdr_color + read/write depth.
        const std::array resources = {
            framework::RGResourceUsage{
                ctx.hdr_color,
                framework::RGAccessType::Write,
                framework::RGStage::ColorAttachment,
            },
            framework::RGResourceUsage{
                ctx.depth,
                framework::RGAccessType::ReadWrite,
                framework::RGStage::DepthAttachment,
            },
        };

        rg.add_pass("Forward", resources,
                    [this, &rg, &ctx](const rhi::CommandBuffer &cmd) {
                        // Color attachment: HDR color buffer
                        const auto hdr_handle = rg.get_image(ctx.hdr_color);
                        VkRenderingAttachmentInfo color_attachment{};
                        color_attachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
                        color_attachment.imageView = rm_->get_image(hdr_handle).view;
                        color_attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                        color_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
                        color_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
                        color_attachment.clearValue.color = {{0.0f, 0.0f, 0.0f, 1.0f}};

                        // Depth attachment
                        const auto depth_handle = rg.get_image(ctx.depth);
                        VkRenderingAttachmentInfo depth_attachment{};
                        depth_attachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
                        depth_attachment.imageView = rm_->get_image(depth_handle).view;
                        depth_attachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
                        depth_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
                        depth_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
                        depth_attachment.clearValue.depthStencil = {0.0f, 0};

                        // Derive render area from the HDR color image dimensions
                        const auto &hdr_image = rm_->get_image(hdr_handle);
                        const VkExtent2D render_extent{hdr_image.desc.width, hdr_image.desc.height};

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
                        cmd.set_depth_write_enable(true);
                        cmd.set_depth_compare_op(VK_COMPARE_OP_GREATER);

                        // Draw visible opaque instances, then visible transparent (back-to-front)
                        auto draw_instance = [&](const uint32_t idx) {
                            const auto &instance = ctx.mesh_instances[idx];
                            const auto &mesh = ctx.meshes[instance.mesh_id];
                            const auto &material = ctx.materials[instance.material_id];

                            cmd.set_cull_mode(
                                material.double_sided
                                    ? VK_CULL_MODE_NONE
                                    : VK_CULL_MODE_BACK_BIT);

                            const framework::PushConstantData pc{
                                .model = instance.transform,
                                .material_index = material.buffer_offset,
                            };
                            cmd.push_constants(
                                pipeline_.layout,
                                VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                                &pc,
                                sizeof(pc));

                            cmd.bind_vertex_buffer(
                                0,
                                rm_->get_buffer(mesh.vertex_buffer).buffer);
                            cmd.bind_index_buffer(
                                rm_->get_buffer(mesh.index_buffer).buffer,
                                VK_INDEX_TYPE_UINT32);
                            cmd.draw_indexed(mesh.index_count);
                        };

                        for (const auto idx: ctx.cull_result->visible_opaque_indices)
                            draw_instance(idx);
                        for (const auto idx: ctx.cull_result->visible_transparent_indices)
                            draw_instance(idx);

                        cmd.end_rendering();
                    });
    }
} // namespace himalaya::passes
