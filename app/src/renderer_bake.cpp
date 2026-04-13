/**
 * @file renderer_bake.cpp
 * @brief Bake render path: lightmap/probe baker state machine and per-frame dispatch.
 */

#include <himalaya/app/renderer.h>

#include <himalaya/framework/frame_context.h>
#include <himalaya/framework/imgui_backend.h>
#include <himalaya/rhi/commands.h>
#include <himalaya/rhi/descriptors.h>
#include <himalaya/rhi/resources.h>
#include <himalaya/rhi/swapchain.h>

#include <array>

#include <spdlog/spdlog.h>

namespace himalaya::app {
    void Renderer::render_baking(rhi::CommandBuffer &cmd, const RenderInput &input) {
        draw_call_count_ = 0;

        switch (bake_state_) {
            case BakeState::BakingLightmaps:
                // TODO: per-instance lightmap bake loop (Step 9 later items)
                break;

            case BakeState::BakingProbes:
                // TODO: per-probe bake loop (Step 12)
                break;

            case BakeState::Complete:
                // All bake work finished — stay in this state until
                // Application restores the pre-bake RenderMode.
                break;

            case BakeState::Idle:
                // Should not reach here — Application only sets Baking mode
                // after transitioning to BakingLightmaps.
                spdlog::warn("render_baking() called in Idle state");
                break;
        }

        // --- Preview: blit accumulation to managed_hdr_color_ + tonemapping + ImGui ---
        // (filled in when per-instance bake loop is implemented)

        render_graph_.clear();

        const auto swapchain_image = render_graph_.import_image(
            "Swapchain",
            swapchain_image_handles_[input.image_index],
            VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);

        const auto hdr_resource = render_graph_.use_managed_image(
            managed_hdr_color_, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, false);

        // Clear hdr_color to black (no 3D content in bake mode)
        const std::array clear_resources = {
            framework::RGResourceUsage{
                hdr_resource,
                framework::RGAccessType::Write,
                framework::RGStage::Transfer,
            },
        };
        render_graph_.add_pass("Bake Clear HDR", clear_resources,
                               [this](const rhi::CommandBuffer &pass_cmd) {
                                   const auto backing = render_graph_.get_managed_backing_image(
                                       managed_hdr_color_);
                                   const auto &img = resource_manager_->get_image(backing);

                                   VkClearColorValue clear_color{};
                                   clear_color.float32[0] = 0.0f;
                                   clear_color.float32[1] = 0.0f;
                                   clear_color.float32[2] = 0.0f;
                                   clear_color.float32[3] = 1.0f;

                                   VkImageSubresourceRange range{};
                                   range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                                   range.baseMipLevel = 0;
                                   range.levelCount = 1;
                                   range.baseArrayLayer = 0;
                                   range.layerCount = 1;

                                   vkCmdClearColorImage(pass_cmd.handle(), img.image,
                                                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                                        &clear_color, 1, &range);
                               });

        // TODO: blit accumulation buffer into hdr_color (centered, aspect-ratio preserved)
        // when a bake instance is active.

        // Update Set 2 binding 0 with hdr_color for tonemapping
        const auto hdr_backing = render_graph_.get_managed_backing_image(managed_hdr_color_);
        descriptor_manager_->update_render_target(input.frame_index, 0, hdr_backing, default_sampler_);

        framework::FrameContext frame_ctx{};
        frame_ctx.swapchain = swapchain_image;
        frame_ctx.hdr_color = hdr_resource;
        frame_ctx.frame_index = input.frame_index;
        frame_ctx.frame_number = frame_counter_;

        tonemapping_pass_.record(render_graph_, frame_ctx);

        // ImGui pass
        const std::array imgui_resources = {
            framework::RGResourceUsage{
                swapchain_image,
                framework::RGAccessType::ReadWrite,
                framework::RGStage::ColorAttachment,
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
