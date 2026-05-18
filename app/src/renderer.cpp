/**
 * @file renderer.cpp
 * @brief Renderer core: render() dispatch, fill_common_gpu_data(), accessors.
 */

#include <himalaya/app/renderer.h>

#include <himalaya/framework/scene_data.h>
#include <himalaya/rhi/resources.h>
#include <himalaya/rhi/swapchain.h>

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
        render_path_tracing(cmd, input);
        ++frame_counter_;
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
