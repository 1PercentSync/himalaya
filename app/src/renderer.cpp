/**
 * @file renderer.cpp
 * @brief Renderer core: render() dispatch, fill_common_gpu_data(), accessors.
 */

#include <himalaya/app/renderer.h>

#include <himalaya/framework/frame_context.h>
#include <himalaya/framework/gaussian_splat_data.h>
#include <himalaya/framework/imgui_backend.h>
#include <himalaya/framework/render_graph.h>
#include <himalaya/framework/scene_data.h>
#include <himalaya/rhi/commands.h>
#include <himalaya/rhi/descriptors.h>
#include <himalaya/rhi/resources.h>
#include <himalaya/rhi/swapchain.h>

#include <algorithm>
#include <array>

#include <GLFW/glfw3.h>

namespace himalaya::app {
    // ---- GPU data fill ----

    void Renderer::fill_common_gpu_data(const RenderInput &input) const {
        // --- Fill GlobalUBO ---
        const auto &ubo_buf = resource_manager_->get_buffer(global_ubo_buffers_[input.frame_index]);

        framework::GlobalUniformData ubo_data{};
        ubo_data.view = input.camera.view;
        ubo_data.projection = input.camera.projection;
        ubo_data.view_projection = input.camera.view_projection;
        ubo_data.inv_view_projection = input.camera.inv_view_projection;
        ubo_data.camera_position_and_exposure = glm::vec4(input.camera.position, input.exposure);
        ubo_data.screen_size = glm::vec2(
            static_cast<float>(swapchain_->extent.width),
            static_cast<float>(swapchain_->extent.height));
        ubo_data.time = static_cast<float>(glfwGetTime());
        ubo_data.indirect_intensity = input.indirect_intensity;
        ubo_data.irradiance_cubemap_index = ibl_.irradiance_cubemap_index().index;
        ubo_data.prefiltered_cubemap_index = ibl_.prefiltered_cubemap_index().index;
        ubo_data.brdf_lut_index = ibl_.brdf_lut_index().index;
        ubo_data.prefiltered_mip_count = ibl_.prefiltered_mip_count();
        ubo_data.skybox_cubemap_index = ibl_.skybox_cubemap_index().index;
        ubo_data.ibl_rotation_sin = input.ibl_rotation_sin;
        ubo_data.ibl_rotation_cos = input.ibl_rotation_cos;
        ubo_data.inv_projection = glm::inverse(input.camera.projection);
        ubo_data.inv_view = glm::inverse(input.camera.view);
        ubo_data.frame_index = frame_counter_;

        std::memcpy(ubo_buf.allocation_info.pMappedData, &ubo_data, sizeof(ubo_data));
    }

    // ---- Render dispatch ----

    void Renderer::render(rhi::CommandBuffer &cmd, const RenderInput &input) {
        pending_semaphore_signal_ = {};
        fill_common_gpu_data(input);

        switch (input.render_mode) {
            case framework::RenderMode::PathTracing:
                if (ctx_->rt_supported && scene_as_builder_.tlas_handle().as != VK_NULL_HANDLE) {
                    render_path_tracing(cmd, input);
                } else {
                    render_imgui_only(cmd, input);
                }
                break;
            case framework::RenderMode::GaussianSplatting:
                if (!gaussian_splat_scene_builder_.valid()) {
                    render_imgui_only(cmd, input);
                    break;
                }

                render_gaussian_splatting(cmd, input);
                break;
        }

        ++frame_counter_;
    }

    passes::GaussianSplatGraphResources Renderer::record_gaussian_splat_preprocess(
        const framework::GaussianSplatGpuScene &scene,
        const uint32_t frame_index,
        const passes::GSPushConstants &push_constants) {
        const auto resources = gaussian_splat_pass_resources_.import_scene_resources(render_graph_, scene);

        gaussian_splat_reset_pass_.record(render_graph_, resources);
        gaussian_splat_cull_project_pass_.record(render_graph_,
                                                 resources,
                                                 scene,
                                                 frame_index,
                                                 push_constants);

        // Bitonic consumes the cull/project sort_entries output in place. It does
        // not touch indirect_draw, so the later draw pass must use the
        // cull/project-written instance_count instead of drawing sort_capacity.
        gaussian_splat_bitonic_sort_pass_.record(render_graph_, resources, scene, frame_index);

        return resources;
    }

    void Renderer::render_gaussian_splatting(rhi::CommandBuffer &cmd, const RenderInput &input) {
        const auto &scene = gaussian_splat_scene_builder_.gpu_scene();

        render_graph_.clear();

        const auto swapchain_image = render_graph_.import_image(
            "Swapchain",
            swapchain_image_handles_[input.image_index],
            VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);

        const auto composition_resource = render_graph_.use_managed_image(managed_gs_composition_,
                                                                          VK_IMAGE_LAYOUT_UNDEFINED,
                                                                          false);
        framework::RGResourceId linear_resource{};
        if (scene.metadata.color_space == framework::GaussianSplatColorSpace::SrgbRec709Display) {
            linear_resource = render_graph_.use_managed_image(managed_gs_linear_,
                                                              VK_IMAGE_LAYOUT_UNDEFINED,
                                                              false);
        }

        const float screen_short_side = static_cast<float>(std::min(swapchain_->extent.width,
                                                                    swapchain_->extent.height));
        const passes::GSPushConstants push_constants{
            .total_splat_count = scene.total_splat_count,
            .sort_capacity = scene.sort_capacity,
            .color_space = static_cast<uint32_t>(scene.metadata.color_space),
            .max_sh_degree = scene.metadata.max_sh_degree,
            .near_gs = input.gs_near_plane,
            .max_projected_extent_px = screen_short_side * 0.25f,
            .alpha_discard_threshold = 1.0e-4f,
            .power_discard_threshold = -20.0f,
        };

        const auto gs_resources = record_gaussian_splat_preprocess(scene,
                                                                   input.frame_index,
                                                                   push_constants);
        gaussian_splat_draw_pass_.record(render_graph_,
                                         composition_resource,
                                         gs_resources,
                                         scene,
                                         input.frame_index,
                                         push_constants);

        framework::RGResourceId tonemapping_input = composition_resource;
        rhi::ImageHandle tonemapping_input_backing = render_graph_.get_managed_backing_image(managed_gs_composition_);
        if (scene.metadata.color_space == framework::GaussianSplatColorSpace::SrgbRec709Display) {
            gaussian_splat_color_convert_pass_.record(render_graph_, composition_resource, linear_resource);
            tonemapping_input = linear_resource;
            tonemapping_input_backing = render_graph_.get_managed_backing_image(managed_gs_linear_);
        }

        framework::FrameContext frame_ctx{};
        frame_ctx.swapchain = swapchain_image;
        frame_ctx.hdr_color = tonemapping_input;
        frame_ctx.frame_index = input.frame_index;
        frame_ctx.frame_number = frame_counter_;

        descriptor_manager_->update_render_target(input.frame_index, 0, tonemapping_input_backing, default_sampler_);

        tonemapping_pass_.record(render_graph_, frame_ctx, passes::TonemappingMode::LinearClamp);

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

    void Renderer::render_imgui_only(rhi::CommandBuffer &cmd, const RenderInput &input) {
        render_graph_.clear();

        const auto swapchain_image = render_graph_.import_image(
            "Swapchain",
            swapchain_image_handles_[input.image_index],
            VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);

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
                                   color_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
                                   color_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
                                   color_attachment.clearValue.color = {{0.0f, 0.0f, 0.0f, 1.0f}};

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

    // ---- Accessors ----

    const framework::IBL &Renderer::ibl() const {
        return ibl_;
    }

    rhi::SamplerHandle Renderer::default_sampler() const {
        return default_sampler_;
    }

    const framework::DefaultTextures &Renderer::default_textures() const {
        return default_textures_;
    }

    framework::MaterialSystem &Renderer::material_system() {
        return material_system_;
    }

    framework::Denoiser::SemaphoreSignal Renderer::pending_denoise_signal() const {
        return pending_semaphore_signal_;
    }

    void Renderer::abort_denoise() {
        denoiser_.abort();
        reset_pt_accumulation();
    }

    void Renderer::request_pt_reset() {
        reset_pt_accumulation();
    }

    void Renderer::request_manual_denoise() {
        manual_denoise_requested_ = true;
    }

    void Renderer::reset_pt_accumulation() {
        upload_pending_completion_ = false;
        reference_view_pass_.reset_accumulation();
        ++accumulation_generation_;
        denoised_generation_ = UINT32_MAX;
        last_denoise_trigger_sample_count_ = 0;
        pt_start_time_ = std::chrono::steady_clock::now();
        pt_finish_time_ = pt_start_time_;
    }

    // ---- Denoiser parameter accessors (for DebugUIContext binding) ----

    bool& Renderer::denoise_enabled() { return denoise_enabled_; }
    bool& Renderer::show_denoised() { return show_denoised_; }
    bool& Renderer::auto_denoise() { return auto_denoise_; }
    uint32_t& Renderer::auto_denoise_interval() { return auto_denoise_interval_; }

    uint32_t Renderer::pt_sample_count() const {
        return reference_view_pass_.sample_count();
    }

    float Renderer::pt_elapsed_time() const {
        if (reference_view_pass_.sample_count() == 0) {
            return 0.0f;
        }
        // Freeze timer when target reached
        if (pt_finish_time_ > pt_start_time_) {
            return std::chrono::duration<float>(pt_finish_time_ - pt_start_time_).count();
        }
        const auto now = std::chrono::steady_clock::now();
        return std::chrono::duration<float>(now - pt_start_time_).count();
    }

    framework::DenoiseState Renderer::denoise_state() const {
        return denoiser_.state();
    }

    uint32_t Renderer::last_denoise_trigger_sample_count() const {
        return last_denoise_trigger_sample_count_;
    }

    float Renderer::last_denoise_duration() const {
        return denoiser_.last_denoise_duration();
    }
} // namespace himalaya::app
