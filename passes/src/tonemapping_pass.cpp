/**
 * @file tonemapping_pass.cpp
 * @brief TonemappingPass implementation — pipeline creation and per-frame recording.
 */

#include <himalaya/passes/tonemapping_pass.h>

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

    void TonemappingPass::setup(rhi::Context &ctx,
                                rhi::ResourceManager &rm,
                                rhi::DescriptorManager &dm,
                                rhi::ShaderCompiler &sc,
                                const VkFormat swapchain_format) {
        ctx_ = &ctx;
        rm_ = &rm;
        dm_ = &dm;
        sc_ = &sc;
        swapchain_format_ = swapchain_format;

        create_pipelines();
    }

    void TonemappingPass::rebuild_pipelines() {
        create_pipelines();
    }

    void TonemappingPass::destroy() const {
        pipeline_.destroy(ctx_->device);
    }

    // ---- Pipeline creation ----

    void TonemappingPass::create_pipelines() {
        // Compile shaders first — if compilation fails, keep the old pipeline
        // intact so the renderer can continue with the previous working shaders.
        const auto vert_spirv = sc_->compile_from_file("fullscreen.vert",
                                                       rhi::ShaderStage::Vertex);
        const auto frag_spirv = sc_->compile_from_file("tonemapping.frag",
                                                       rhi::ShaderStage::Fragment);

        if (vert_spirv.empty() || frag_spirv.empty()) {
            spdlog::warn("TonemappingPass: shader compilation failed, keeping previous pipeline");
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
        desc.color_formats = {swapchain_format_};
        desc.depth_format = VK_FORMAT_UNDEFINED; // No depth
        desc.sample_count = 1; // Always 1x — processes resolved HDR
        // No vertex input (fullscreen triangle from gl_VertexIndex)

        const auto set_layouts = dm_->get_global_set_layouts();
        desc.descriptor_set_layouts = {set_layouts.begin(), set_layouts.end()};

        // No push constants needed for tonemapping

        pipeline_ = rhi::create_graphics_pipeline(ctx_->device, desc);

        vkDestroyShaderModule(ctx_->device, frag_module, nullptr);
        vkDestroyShaderModule(ctx_->device, vert_module, nullptr);
    }

    // ---- Per-frame recording ----

    void TonemappingPass::record(framework::RenderGraph &rg,
                                 const framework::FrameContext &ctx) const {
        // Read hdr_color (Fragment sampler), write swapchain (ColorAttachment).
        const std::array resources = {
            framework::RGResourceUsage{
                ctx.hdr_color,
                framework::RGAccessType::Read,
                framework::RGStage::Fragment,
            },
            framework::RGResourceUsage{
                ctx.swapchain,
                framework::RGAccessType::Write,
                framework::RGStage::ColorAttachment,
            },
        };

        rg.add_pass("Tonemapping", resources,
                    [this, &rg, &ctx](const rhi::CommandBuffer &cmd) {
                        // Color attachment: swapchain image
                        const auto swapchain_handle = rg.get_image(ctx.swapchain);
                        VkRenderingAttachmentInfo color_attachment{};
                        color_attachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
                        color_attachment.imageView = rm_->get_image(swapchain_handle).view;
                        color_attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                        color_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
                        color_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

                        const auto &swapchain_image = rm_->get_image(swapchain_handle);
                        const VkExtent2D render_extent{
                            swapchain_image.desc.width, swapchain_image.desc.height
                        };

                        VkRenderingInfo rendering_info{};
                        rendering_info.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
                        rendering_info.renderArea = {{0, 0}, render_extent};
                        rendering_info.layerCount = 1;
                        rendering_info.colorAttachmentCount = 1;
                        rendering_info.pColorAttachments = &color_attachment;

                        cmd.begin_rendering(rendering_info);
                        cmd.bind_pipeline(pipeline_);

                        // Bind Set 0 (GlobalUBO for exposure) + Set 1 + Set 2 (hdr_color).
                        const std::array sets = {
                            dm_->get_set0(ctx.frame_index),
                            dm_->get_set1(),
                            dm_->get_set2(ctx.frame_index),
                        };
                        cmd.bind_descriptor_sets(pipeline_.layout, 0, sets.data(), static_cast<uint32_t>(sets.size()));

                        // Normal viewport (no Y-flip): fullscreen post-processing
                        // samples a texture, no 3D coordinate convention to fix.
                        VkViewport viewport{};
                        viewport.x = 0.0f;
                        viewport.y = 0.0f;
                        viewport.width = static_cast<float>(render_extent.width);
                        viewport.height = static_cast<float>(render_extent.height);
                        viewport.minDepth = 0.0f;
                        viewport.maxDepth = 1.0f;
                        cmd.set_viewport(viewport);
                        cmd.set_scissor({{0, 0}, render_extent});

                        // Dynamic state defaults for fullscreen pass
                        cmd.set_cull_mode(VK_CULL_MODE_NONE);
                        cmd.set_depth_test_enable(false);
                        cmd.set_depth_write_enable(false);

                        cmd.draw(3); // Fullscreen triangle

                        cmd.end_rendering();
                    });
    }
} // namespace himalaya::passes
