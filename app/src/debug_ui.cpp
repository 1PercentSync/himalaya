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
    bool slider_float_deferred(const char* label, float* v, const float v_min, const float v_max,
                               const char* format, const ImGuiSliderFlags flags = 0) {
        const float original = *v;
        ImGui::SliderFloat(label, v, v_min, v_max, format, flags);

        if (ImGui::IsItemActive() && ImGui::GetIO().WantTextInput) {
            *v = original;
            return false;
        }

        return *v != original;
    }

    /** SliderAngle variant with the same deferred text-input behaviour. */
    bool slider_angle_deferred(const char* label, float* v_rad,
                               const float v_degrees_min, const float v_degrees_max,
                               const char* format, const ImGuiSliderFlags flags = 0) {
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
        for (const float s : samples_) total += s;

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

    DebugUIActions DebugUI::draw(const DebugUIContext& ctx) {
        DebugUIActions actions;

        frame_stats_.push(ctx.delta_time);

        ImGui::SetNextWindowPos({0, 0}, ImGuiCond_Once);
        ImGui::Begin("Debug", nullptr, ImGuiWindowFlags_AlwaysAutoResize);

        ImGui::Text("FPS: %.1f (%.2f ms)", frame_stats_.avg_fps, frame_stats_.avg_frame_time_ms);
        ImGui::Text("1%% Low: %.1f (%.2f ms)", frame_stats_.low1_fps, frame_stats_.low1_frame_time_ms);

        ImGui::Separator();
        ImGui::Text("GPU: %s", ctx.context.gpu_name.c_str());
        ImGui::Text("Resolution: %u x %u", ctx.swapchain.extent.width, ctx.swapchain.extent.height);

        const auto vram = ctx.context.query_vram_usage();
        ImGui::Text("VRAM: %.1f / %.1f MB",
                    static_cast<double>(vram.used) / (1024.0 * 1024.0),
                    static_cast<double>(vram.budget) / (1024.0 * 1024.0));

        ImGui::Separator();
        if (ImGui::Checkbox("VSync", &ctx.swapchain.vsync)) {
            actions.vsync_toggled = true;
        }

        int current_log_level = static_cast<int>(spdlog::get_level());
        constexpr const char* kLogLevelNames[] = {"Trace", "Debug", "Info", "Warn", "Error", "Critical", "Off"};
        if (ImGui::Combo("Log Level", &current_log_level, kLogLevelNames, IM_ARRAYSIZE(kLogLevelNames))) {
            spdlog::set_level(static_cast<spdlog::level::level_enum>(current_log_level));
        }

        // Camera section
        ImGui::Separator();
        if (ImGui::CollapsingHeader("Camera", ImGuiTreeNodeFlags_DefaultOpen)) {
            const auto& pos = ctx.camera.position;
            ImGui::Text("Pos: (%.2f, %.2f, %.2f)", pos.x, pos.y, pos.z);
            ImGui::Text("Yaw: %.1f%s  Pitch: %.1f%s",
                        glm::degrees(ctx.camera.yaw), "\xC2\xB0",
                        glm::degrees(ctx.camera.pitch), "\xC2\xB0");

            slider_angle_deferred("FOV", &ctx.camera.fov, 30.0f, 120.0f, "%.1f\xC2\xB0");
            slider_float_deferred("Near", &ctx.camera.near_plane, 0.01f, 10.0f, "%.2f",
                                  ImGuiSliderFlags_Logarithmic);
            slider_float_deferred("Far", &ctx.camera.far_plane, 10.0f, 10000.0f, "%.1f",
                                  ImGuiSliderFlags_Logarithmic);
        }

        // Scene statistics section
        ImGui::Separator();
        if (ImGui::CollapsingHeader("Scene", ImGuiTreeNodeFlags_DefaultOpen)) {
            const auto& stats = ctx.scene_stats;
            ImGui::Text("Instances: %u", stats.total_instances);
            ImGui::Text("Meshes: %u  Materials: %u", stats.total_meshes, stats.total_materials);
            ImGui::Text("Visible: %u opaque, %u transparent",
                        stats.visible_opaque, stats.visible_transparent);
            ImGui::Text("Culled: %u", stats.culled);
            ImGui::Text("Draw calls: %u", stats.draw_calls);
            ImGui::Text("Triangles: %u", stats.rendered_triangles);
        }

        // Lighting section
        ImGui::Separator();
        if (ImGui::CollapsingHeader("Lighting", ImGuiTreeNodeFlags_DefaultOpen)) {
            const bool using_default = !ctx.has_scene_lights || ctx.force_default_light;

            // Force Default Light checkbox (disabled when scene has no lights)
            ImGui::BeginDisabled(!ctx.has_scene_lights);
            ImGui::Checkbox("Force Default Light", &ctx.force_default_light);
            ImGui::EndDisabled();

            ImGui::Text("Yaw: %.1f%s  Pitch: %.1f%s",
                        ctx.light_yaw_deg, "\xC2\xB0",
                        ctx.light_pitch_deg, "\xC2\xB0");

            // Intensity slider (editable only when using default light)
            if (using_default) {
                slider_float_deferred("Intensity", &ctx.default_intensity,
                                      0.0f, 10.0f, "%.2f");
            } else {
                ImGui::BeginDisabled();
                float display = ctx.light_intensity;
                ImGui::SliderFloat("Intensity", &display, 0.0f, 10.0f, "%.2f");
                ImGui::EndDisabled();
            }
        }

        // Rendering section
        ImGui::Separator();
        if (ImGui::CollapsingHeader("Rendering", ImGuiTreeNodeFlags_DefaultOpen)) {
            slider_float_deferred("Ambient", &ctx.ambient_intensity, 0.0f, 1.0f, "%.3f");
            slider_float_deferred("Exposure", &ctx.exposure, 0.1f, 10.0f, "%.2f",
                                  ImGuiSliderFlags_Logarithmic);
        }

        ImGui::End();

        return actions;
    }

} // namespace himalaya::app
