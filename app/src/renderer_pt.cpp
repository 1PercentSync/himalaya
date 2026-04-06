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

        // --- Deferred upload completion from previous frame ---
        // complete_upload() is deferred until the next frame so the GPU has
        // actually executed the upload pass (begin_frame fence wait guarantees this).
        // Trade-off: minimum denoise trigger interval increases from 2 to 3 frames
        // (trigger → upload → complete/re-trigger). Negligible at interval ≥ 16.
        if (upload_pending_completion_) {
            denoiser_.complete_upload();
            denoised_generation_ = pending_denoised_generation_;
            upload_pending_completion_ = false;
        }

        // --- Build render graph ---
        render_graph_.clear();

        const auto swapchain_image = render_graph_.import_image(
            "Swapchain",
            swapchain_image_handles_[input.image_index],
            VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);

        // Use accumulation buffer as hdr_color for TonemappingPass
        // Preserve content only when there are previous samples to accumulate on.
        // sample_count==0 (first frame or after reset): shader overwrites entirely → UNDEFINED ok.
        // sample_count>0: shader does imageLoad for running average → must preserve.
        const bool accum_has_data = reference_view_pass_.sample_count() > 0;
        const auto accum_resource = render_graph_.use_managed_image(managed_pt_accumulation_,
                                                                    VK_IMAGE_LAYOUT_GENERAL,
                                                                    accum_has_data);

        // Aux images are fully overwritten each frame (bounce 0 imageStore)
        const auto aux_albedo_resource = render_graph_.use_managed_image(managed_pt_aux_albedo_,
                                                                         VK_IMAGE_LAYOUT_GENERAL,
                                                                         false);
        const auto aux_normal_resource = render_graph_.use_managed_image(managed_pt_aux_normal_,
                                                                         VK_IMAGE_LAYOUT_GENERAL,
                                                                         false);

        // --- Construct FrameContext ---
        framework::FrameContext frame_ctx{};
        frame_ctx.swapchain = swapchain_image;
        frame_ctx.hdr_color = accum_resource; // Tonemapping reads from accumulation
        frame_ctx.pt_accumulation = accum_resource;
        frame_ctx.pt_aux_albedo = aux_albedo_resource;
        frame_ctx.pt_aux_normal = aux_normal_resource;
        frame_ctx.frame_index = input.frame_index;
        frame_ctx.frame_number = frame_counter_;

        // --- VP / IBL rotation / light comparison for accumulation reset ---
        const auto light_count = static_cast<uint32_t>(input.lights.size());
        glm::vec4 light_dir_intensity{0.0f};
        glm::vec4 light_color_shadow{0.0f};
        if (light_count > 0) {
            const auto &l = input.lights[0];
            light_dir_intensity = glm::vec4(l.direction, l.intensity);
            light_color_shadow = glm::vec4(l.color, l.cast_shadows ? 1.0f : 0.0f);
        }

        if (input.camera.view_projection != prev_pt_view_projection_ ||
            input.ibl_rotation_sin != prev_pt_ibl_rotation_sin_ ||
            input.ibl_rotation_cos != prev_pt_ibl_rotation_cos_ ||
            light_count != prev_pt_light_count_ ||
            light_dir_intensity != prev_pt_light_dir_intensity_ ||
            light_color_shadow != prev_pt_light_color_shadow_ ||
            max_bounces_ != prev_max_bounces_ ||
            max_clamp_ != prev_max_clamp_ ||
            env_sampling_ != prev_env_sampling_) {
            reset_pt_accumulation();
        }
        prev_pt_view_projection_ = input.camera.view_projection;
        prev_pt_ibl_rotation_sin_ = input.ibl_rotation_sin;
        prev_pt_ibl_rotation_cos_ = input.ibl_rotation_cos;
        prev_pt_light_count_ = light_count;
        prev_pt_light_dir_intensity_ = light_dir_intensity;
        prev_pt_light_color_shadow_ = light_color_shadow;
        prev_max_bounces_ = max_bounces_;
        prev_max_clamp_ = max_clamp_;
        prev_env_sampling_ = env_sampling_;

        // --- Denoise trigger guard ---
        if (const uint32_t sample_count = reference_view_pass_.sample_count();
            denoiser_.state() == framework::DenoiseState::Idle &&
            denoise_enabled_ && show_denoised_ && sample_count > 0) {
            const bool auto_trigger = auto_denoise_ &&
                                      (sample_count - last_denoise_trigger_sample_count_ >= auto_denoise_interval_);
            const bool manual_trigger = manual_denoise_requested_;
            manual_denoise_requested_ = false;
            if (auto_trigger || manual_trigger) {
                last_denoise_trigger_sample_count_ = sample_count;
                denoiser_.request_denoise(accumulation_generation_);
            }
        }

        // --- Denoised buffer: single use_managed_image per frame ---
        // GENERAL as consistent cross-frame "home" layout.
        // RG transitions: Upload frame UNDEFINED→TRANSFER_DST→...→GENERAL,
        //                 Display frame GENERAL→SHADER_READ_ONLY→GENERAL.
        const bool uploading = denoiser_.poll_upload_ready(accumulation_generation_);
        const bool want_display = show_denoised_ && denoise_enabled_;
        const bool have_valid = denoised_generation_ == accumulation_generation_;

        framework::RGResourceId denoised_resource{};
        if (uploading || (want_display && have_valid)) {
            denoised_resource = render_graph_.use_managed_image(
                managed_denoised_, VK_IMAGE_LAYOUT_GENERAL, !uploading);
        }

        // --- Upload pass ---
        if (uploading) {
            const std::array upload_resources = {
                framework::RGResourceUsage{
                    denoised_resource,
                    framework::RGAccessType::Write,
                    framework::RGStage::Transfer
                },
            };
            render_graph_.add_pass("OIDN Upload",
                                   upload_resources,
                                   [this](const rhi::CommandBuffer &pass_cmd) {
                                       const auto &denoised_img = resource_manager_->get_image(
                                           render_graph_.get_managed_backing_image(managed_denoised_));
                                       const auto &upload_buf = resource_manager_->get_buffer(
                                           denoiser_.upload_buffer());

                                       VkBufferImageCopy region{};
                                       region.bufferOffset = 0;
                                       region.bufferRowLength = 0;
                                       region.bufferImageHeight = 0;
                                       region.imageSubresource = {
                                           VK_IMAGE_ASPECT_COLOR_BIT,
                                           0,
                                           0,
                                           1
                                       };
                                       region.imageOffset = {0, 0, 0};
                                       region.imageExtent = {denoiser_.width(), denoiser_.height(), 1};

                                       vkCmdCopyBufferToImage(pass_cmd.handle(),
                                                              upload_buf.buffer, denoised_img.image,
                                                              VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                                              1, &region);
                                   });

            // Defer complete_upload() to next frame — the upload pass is only
            // recorded at this point, not yet executed by the GPU. The next
            // frame's begin_frame() fence wait guarantees GPU completion.
            upload_pending_completion_ = true;
            pending_denoised_generation_ = accumulation_generation_;
        }

        // --- Tonemapping input: denoised buffer or raw accumulation ---
        // Check pending completion too: upload was recorded this frame, denoised
        // buffer content will be valid after GPU executes, safe to display.
        const bool upload_committed = upload_pending_completion_ &&
                                      pending_denoised_generation_ == accumulation_generation_;
        const bool use_denoised = want_display
                                  && (denoised_generation_ == accumulation_generation_ || upload_committed)
                                  && denoised_resource.valid();
        if (use_denoised) {
            frame_ctx.hdr_color = denoised_resource;
        }

        // Update Set 2 binding 0 once with the final tonemapping source
        const auto hdr_backing = render_graph_.get_managed_backing_image(
            use_denoised ? managed_denoised_ : managed_pt_accumulation_);
        descriptor_manager_->update_render_target(input.frame_index, 0, hdr_backing, default_sampler_);

        // --- Record passes ---
        reference_view_pass_.set_max_bounces(max_bounces_);
        reference_view_pass_.set_max_clamp(max_clamp_);
        reference_view_pass_.set_env_sampling(env_sampling_);

        // Skip accumulation when target sample count is reached
        if (target_samples_ == 0 || reference_view_pass_.sample_count() < target_samples_) {
            reference_view_pass_.record(render_graph_, frame_ctx);

            // Record finish time on the frame that reaches the target
            if (target_samples_ > 0 &&
                reference_view_pass_.sample_count() >= target_samples_ &&
                pt_finish_time_ <= pt_start_time_) {
                pt_finish_time_ = std::chrono::steady_clock::now();
            }
        }

        // --- Readback copy pass (after Reference View, before compile) ---
        if (denoiser_.state() == framework::DenoiseState::ReadbackPending) {
            const std::array readback_resources = {
                framework::RGResourceUsage{
                    accum_resource,
                    framework::RGAccessType::Read,
                    framework::RGStage::Transfer
                },
                framework::RGResourceUsage{
                    aux_albedo_resource,
                    framework::RGAccessType::Read,
                    framework::RGStage::Transfer
                },
                framework::RGResourceUsage{
                    aux_normal_resource,
                    framework::RGAccessType::Read,
                    framework::RGStage::Transfer
                },
            };
            render_graph_.add_pass("OIDN Readback", readback_resources,
                                   [this](const rhi::CommandBuffer &pass_cmd) {
                                       const auto &accum_img = resource_manager_->get_image(
                                           render_graph_.get_managed_backing_image(managed_pt_accumulation_));
                                       const auto &albedo_img = resource_manager_->get_image(
                                           render_graph_.get_managed_backing_image(managed_pt_aux_albedo_));
                                       const auto &normal_img = resource_manager_->get_image(
                                           render_graph_.get_managed_backing_image(managed_pt_aux_normal_));

                                       const auto &rb_beauty = resource_manager_->get_buffer(
                                           denoiser_.readback_beauty_buffer());
                                       const auto &rb_albedo = resource_manager_->get_buffer(
                                           denoiser_.readback_albedo_buffer());
                                       const auto &rb_normal = resource_manager_->get_buffer(
                                           denoiser_.readback_normal_buffer());

                                       constexpr VkImageSubresourceLayers subresource = {
                                           VK_IMAGE_ASPECT_COLOR_BIT,
                                           0,
                                           0,
                                           1
                                       };
                                       const VkExtent3D extent = {denoiser_.width(), denoiser_.height(), 1};

                                       VkBufferImageCopy region{};
                                       region.bufferOffset = 0;
                                       region.bufferRowLength = 0;
                                       region.bufferImageHeight = 0;
                                       region.imageSubresource = subresource;
                                       region.imageOffset = {0, 0, 0};
                                       region.imageExtent = extent;

                                       vkCmdCopyImageToBuffer(pass_cmd.handle(),
                                                              accum_img.image,
                                                              VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                                              rb_beauty.buffer,
                                                              1,
                                                              &region);
                                       vkCmdCopyImageToBuffer(pass_cmd.handle(),
                                                              albedo_img.image,
                                                              VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                                              rb_albedo.buffer,
                                                              1,
                                                              &region);
                                       vkCmdCopyImageToBuffer(pass_cmd.handle(),
                                                              normal_img.image,
                                                              VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                                              rb_normal.buffer,
                                                              1,
                                                              &region);
                                   });

            pending_semaphore_signal_ = denoiser_.launch_processing();
        }

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
