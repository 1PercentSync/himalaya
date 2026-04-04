/**
 * @file renderer_pt.cpp
 * @brief Path tracing render path: Reference View Pass + Tonemapping + ImGui.
 */

#include <himalaya/app/renderer.h>

#include <himalaya/framework/frame_context.h>
#include <himalaya/framework/imgui_backend.h>
#include <himalaya/rhi/commands.h>
#include <himalaya/rhi/descriptors.h>
#include <himalaya/rhi/resources.h>
#include <himalaya/rhi/swapchain.h>

#include <array>

namespace himalaya::app {
    void Renderer::render_path_tracing(rhi::CommandBuffer &cmd, const RenderInput &input) {
        draw_call_count_ = 0;

        // --- Build render graph ---
        render_graph_.clear();

        const auto swapchain_image = render_graph_.import_image(
            "Swapchain",
            swapchain_image_handles_[input.image_index],
            VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);

        // Use accumulation buffer as hdr_color for TonemappingPass
        const auto accum_resource = render_graph_.use_managed_image(
            managed_pt_accumulation_, VK_IMAGE_LAYOUT_GENERAL);

        // Update Set 2 binding 0 to point to accumulation buffer for Tonemapping sampling
        const auto accum_backing = render_graph_.get_managed_backing_image(managed_pt_accumulation_);
        descriptor_manager_->update_render_target(input.frame_index, 0,
                                                   accum_backing, default_sampler_);

        const auto aux_albedo_resource = render_graph_.use_managed_image(
            managed_pt_aux_albedo_, VK_IMAGE_LAYOUT_GENERAL);
        const auto aux_normal_resource = render_graph_.use_managed_image(
            managed_pt_aux_normal_, VK_IMAGE_LAYOUT_GENERAL);

        // --- Construct FrameContext ---
        framework::FrameContext frame_ctx{};
        frame_ctx.swapchain = swapchain_image;
        frame_ctx.hdr_color = accum_resource; // Tonemapping reads from accumulation
        frame_ctx.pt_accumulation = accum_resource;
        frame_ctx.pt_aux_albedo = aux_albedo_resource;
        frame_ctx.pt_aux_normal = aux_normal_resource;
        frame_ctx.frame_index = input.frame_index;
        frame_ctx.frame_number = frame_counter_;

        // --- VP comparison for accumulation reset ---
        if (input.camera.view_projection != prev_pt_view_projection_) {
            reference_view_pass_.reset_accumulation();
        }
        prev_pt_view_projection_ = input.camera.view_projection;

        // --- Record passes ---
        reference_view_pass_.record(render_graph_, frame_ctx);
        tonemapping_pass_.record(render_graph_, frame_ctx);

        // --- ImGui pass ---
        const std::array imgui_resources = {
            framework::RGResourceUsage{
                swapchain_image,
                framework::RGAccessType::ReadWrite,
                framework::RGStage::ColorAttachment
            },
        };
        render_graph_.add_pass("ImGui", imgui_resources,
                               [this, &input](const rhi::CommandBuffer &pass_cmd) {
                                   VkRenderingAttachmentInfo color_attachment{};
                                   color_attachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
                                   color_attachment.imageView = swapchain_->image_views[input.image_index];
                                   color_attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                                   color_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
                                   color_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

                                   VkRenderingInfo rendering_info{};
                                   rendering_info.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
                                   rendering_info.renderArea = {{0, 0}, swapchain_->extent};
                                   rendering_info.layerCount = 1;
                                   rendering_info.colorAttachmentCount = 1;
                                   rendering_info.pColorAttachments = &color_attachment;

                                   pass_cmd.begin_rendering(rendering_info);
                                   imgui_->render(pass_cmd.handle());
                                   pass_cmd.end_rendering();
                               });

        render_graph_.compile();
        render_graph_.execute(cmd);
    }
} // namespace himalaya::app
