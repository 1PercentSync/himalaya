/**
 * @file debug_ui.cpp
 * @brief DebugUI implementation: frame stats computation and ImGui panel drawing.
 */

#include <himalaya/app/debug_ui.h>

#include <himalaya/framework/camera.h>
#include <himalaya/rhi/context.h>
#include <himalaya/rhi/swapchain.h>

#include <algorithm>

#include <glm/trigonometric.hpp>
#include <imgui.h>
#include <spdlog/spdlog.h>

namespace {
    /**
     * SliderFloat that applies immediately during mouse drag but defers
     * Ctrl+Click text-input changes until Enter / click-away / Tab.
     * During typing, the underlying value stays at its pre-edit state so
     * the renderer does not see intermediate keystrokes.
     */
    bool slider_float_deferred(const char *label,
                               float *v,
                               const float v_min,
                               const float v_max,
                               const char *format,
                               const ImGuiSliderFlags flags = 0) {
        const float original = *v;
        ImGui::SliderFloat(label, v, v_min, v_max, format, flags);

        if (ImGui::IsItemActive() && ImGui::GetIO().WantTextInput) {
            *v = original;
            return false;
        }

        return *v != original;
    }

    /** SliderAngle variant with the same deferred text-input behaviour. */
    // ReSharper disable CppDFAConstantParameter
    bool slider_angle_deferred(const char *label,
                               float *v_rad,
                               const float v_degrees_min,
                               const float v_degrees_max,
                               const char *format,
                               const ImGuiSliderFlags flags = 0) {
        // ReSharper restore CppDFAConstantParameter
        const float original = *v_rad;
        ImGui::SliderAngle(label, v_rad, v_degrees_min, v_degrees_max, format, flags);

        if (ImGui::IsItemActive() && ImGui::GetIO().WantTextInput) {
            *v_rad = original;
            return false;
        }

        return *v_rad != original;
    }
} // anonymous namespace

namespace himalaya::app {
    // ---- FrameStats ----

    void DebugUI::FrameStats::push(const float delta_time) {
        samples_.push_back(delta_time);
        elapsed_ += delta_time;

        if (elapsed_ >= kUpdateInterval) {
            compute();
            samples_.clear();
            elapsed_ = 0.0f;
        }
    }

    void DebugUI::FrameStats::compute() {
        const size_t n = samples_.size();
        if (n == 0) return;

        float total = 0.0f;
        for (const float s: samples_) {
            total += s;
        }

        avg_frame_time_ms = (total / static_cast<float>(n)) * 1000.0f;
        avg_fps = static_cast<float>(n) / total;

        // 1% low: average the worst (longest) 1% of frame times
        std::ranges::sort(samples_, std::greater<>());
        const size_t low_count = std::max<size_t>(1, n / 100);

        float low_total = 0.0f;
        for (size_t i = 0; i < low_count; ++i) {
            low_total += samples_[i];
        }
        low1_frame_time_ms = (low_total / static_cast<float>(low_count)) * 1000.0f;
        low1_fps = 1000.0f / low1_frame_time_ms;
    }

    // ---- DebugUI ----

    DebugUIActions DebugUI::draw(const DebugUIContext &ctx) {
        DebugUIActions actions;

        frame_stats_.push(ctx.delta_time);

        ImGui::SetNextWindowPos({0, 0}, ImGuiCond_Once);
        ImGui::Begin("Debug", nullptr, ImGuiWindowFlags_AlwaysAutoResize);

        ImGui::Text("FPS: %.1f (%.2f ms)", frame_stats_.avg_fps, frame_stats_.avg_frame_time_ms);
        ImGui::Text("1%% Low: %.1f (%.2f ms)", frame_stats_.low1_fps, frame_stats_.low1_frame_time_ms);

        ImGui::Separator();
        ImGui::Text("GPU: %s", ctx.context.gpu_name.c_str());
        ImGui::Text("Resolution: %u x %u", ctx.swapchain.extent.width, ctx.swapchain.extent.height);

        // ReSharper disable once CppUseStructuredBinding
        const auto vram = ctx.context.query_vram_usage();
        ImGui::Text("VRAM: %.1f / %.1f MB",
                    static_cast<double>(vram.used) / (1024.0 * 1024.0),
                    static_cast<double>(vram.budget) / (1024.0 * 1024.0));

        ImGui::Separator();
        if (ImGui::Checkbox("VSync", &ctx.swapchain.vsync)) {
            actions.vsync_toggled = true;
        }

        int current_log_level = spdlog::get_level();
        constexpr const char *kLogLevelNames[] = {"Trace", "Debug", "Info", "Warn", "Error", "Critical", "Off"};
        if (ImGui::Combo("Log Level",
                         &current_log_level,
                         kLogLevelNames,
                         IM_ARRAYSIZE(kLogLevelNames))) {
            spdlog::set_level(static_cast<spdlog::level::level_enum>(current_log_level));
        }

        // Camera section
        ImGui::Separator();
        if (ImGui::CollapsingHeader("Camera", ImGuiTreeNodeFlags_DefaultOpen)) {
            const auto &pos = ctx.camera.position;
            ImGui::Text("Pos: (%.2f, %.2f, %.2f)", pos.x, pos.y, pos.z);
            ImGui::Text("Yaw: %.1f%s  Pitch: %.1f%s",
                        glm::degrees(ctx.camera.yaw), "\xC2\xB0",
                        glm::degrees(ctx.camera.pitch), "\xC2\xB0");

            slider_angle_deferred("FOV",
                                  &ctx.camera.fov,
                                  30.0f,
                                  120.0f,
                                  "%.1f\xC2\xB0");
            slider_float_deferred("Near",
                                  &ctx.camera.near_plane,
                                  0.01f,
                                  10.0f,
                                  "%.2f",
                                  ImGuiSliderFlags_Logarithmic);
            slider_float_deferred("Far",
                                  &ctx.camera.far_plane,
                                  10.0f,
                                  10000.0f,
                                  "%.1f",
                                  ImGuiSliderFlags_Logarithmic);
        }

        // Scene statistics section
        ImGui::Separator();
        if (ImGui::CollapsingHeader("Scene", ImGuiTreeNodeFlags_DefaultOpen)) {
            // ReSharper disable once CppUseStructuredBinding
            const auto &stats = ctx.scene_stats;
            ImGui::Text("Instances: %u", stats.total_instances);
            ImGui::Text("Meshes: %u  Materials: %u  Textures: %u",
                        stats.total_meshes, stats.total_materials, stats.total_textures);
            ImGui::Text("Vertices: %u", stats.total_vertices);
            ImGui::Text("Visible: %u opaque, %u transparent",
                        stats.visible_opaque, stats.visible_transparent);
            ImGui::Text("Culled: %u", stats.culled);
            ImGui::Text("Draw calls: %u", stats.draw_calls);
            ImGui::Text("Triangles: %u", stats.rendered_triangles);
        }

        // Lighting section
        ImGui::Separator();
        if (ImGui::CollapsingHeader("Lighting", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Text("Active Lights: %u", ctx.active_light_count);
            ImGui::Text("IBL Rotation: %.1f%s", ctx.ibl_rotation_deg, "\xC2\xB0");
            ImGui::Checkbox("Disable Scene Lights", &ctx.disable_scene_lights);
        }

        // Rendering section
        ImGui::Separator();
        if (ImGui::CollapsingHeader("Rendering", ImGuiTreeNodeFlags_DefaultOpen)) {
            // MSAA combo box (1x/2x/4x/8x filtered by GPU support)
            constexpr uint32_t kSampleCounts[] = {1, 2, 4, 8};
            constexpr const char *kSampleLabels[] = {"1x", "2x", "4x", "8x"};
            static_assert(IM_ARRAYSIZE(kSampleCounts) == IM_ARRAYSIZE(kSampleLabels));

            int current_idx = 0;
            for (int i = 0; i < IM_ARRAYSIZE(kSampleCounts); ++i) {
                if (kSampleCounts[i] == ctx.current_sample_count) {
                    current_idx = i;
                    break;
                }
            }

            if (ImGui::BeginCombo("MSAA", kSampleLabels[current_idx])) {
                for (int i = 0; i < IM_ARRAYSIZE(kSampleCounts); ++i) {
                    if (!(ctx.supported_sample_counts & kSampleCounts[i])) continue;
                    const bool is_selected = (i == current_idx);
                    if (ImGui::Selectable(kSampleLabels[i], is_selected)) {
                        if (kSampleCounts[i] != ctx.current_sample_count) {
                            actions.msaa_changed = true;
                            actions.new_sample_count = kSampleCounts[i];
                        }
                    }
                    if (is_selected) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }

            slider_float_deferred("IBL Intensity", &ctx.ibl_intensity, 0.0f, 5.0f, "%.2f");
            slider_float_deferred("EV", &ctx.ev, -4.0f, 4.0f, "%.1f");
        }

        ImGui::End();

        return actions;
    }
} // namespace himalaya::app
