/**
 * @file debug_ui.cpp
 * @brief DebugUI implementation: frame stats computation and ImGui panel drawing.
 */

#include <himalaya/app/debug_ui.h>

#include <himalaya/framework/cache.h>
#include <himalaya/framework/camera.h>
#include <himalaya/framework/scene_data.h>
#include <himalaya/rhi/context.h>
#include <himalaya/rhi/swapchain.h>

#include <algorithm>
#include <cmath>
#include <filesystem>

#include <glm/trigonometric.hpp>
#include <imgui.h>
#include <spdlog/spdlog.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <commdlg.h>
#endif

namespace {
#ifdef _WIN32
    /**
     * Opens a Windows native file dialog and returns the selected path.
     * Returns empty string if cancelled. filter uses the GetOpenFileName
     * double-null-terminated format: "Description\0*.ext1;*.ext2\0\0"
     */
    // ReSharper disable CppDFAConstantParameter
    std::string open_file_dialog(const wchar_t *filter, const wchar_t *title) {
        // ReSharper restore CppDFAConstantParameter
        wchar_t file_path[MAX_PATH] = {};
        OPENFILENAMEW ofn = {};
        ofn.lStructSize = sizeof(ofn);
        ofn.lpstrFilter = filter;
        ofn.lpstrFile = file_path;
        ofn.nMaxFile = MAX_PATH;
        ofn.lpstrTitle = title;
        ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;

        if (GetOpenFileNameW(&ofn)) {
            return std::filesystem::path(file_path).string();
        }
        return {};
    }
#endif

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

    // ReSharper disable once CppParameterMayBeConstPtrOrRef
    DebugUIActions DebugUI::draw(DebugUIContext &ctx) {
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

        // Error banner (dismissable)
        if (!ctx.error_message.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.4f, 0.4f, 1.0f));
            ImGui::TextWrapped("%s", ctx.error_message.c_str());
            ImGui::PopStyleColor();
            ImGui::SameLine();
            if (ImGui::SmallButton("X")) {
                actions.error_dismissed = true;
            }
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

        // Scene section (file loading + statistics)
        ImGui::Separator();
        if (ImGui::CollapsingHeader("Scene", ImGuiTreeNodeFlags_DefaultOpen)) {
            // Current scene file
            if (ctx.scene_path.empty()) {
                ImGui::TextDisabled("No scene loaded");
            } else {
                const auto filename = std::filesystem::path(ctx.scene_path).filename().string();
                ImGui::Text("Scene: %s", filename.c_str());
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("%s", ctx.scene_path.c_str());
                }
            }

#ifdef _WIN32
            ImGui::SameLine();
            if (ImGui::Button("Load Scene...")) {
                auto path = open_file_dialog(
                    L"glTF Files (*.gltf;*.glb)\0*.gltf;*.glb\0All Files (*.*)\0*.*\0",
                    L"Load Scene");
                if (!path.empty()) {
                    actions.scene_load_requested = true;
                    actions.new_scene_path = std::move(path);
                }
            }
#endif

            ImGui::Separator();

            // ReSharper disable once CppUseStructuredBinding
            const auto &stats = ctx.scene_stats;

            // Scene assets
            ImGui::Text("Instances: %u  Meshes: %u",
                        stats.total_instances, stats.total_meshes);
            ImGui::Text("Materials: %u  Textures: %u",
                        stats.total_materials, stats.total_textures);
            ImGui::Text("Vertices: %u", stats.total_vertices);

            // Per-frame rendering stats
            ImGui::Separator();
            ImGui::Text("Visible: %u opaque, %u transparent",
                        stats.visible_opaque, stats.visible_transparent);
            ImGui::Text("Culled: %u", stats.culled);
            ImGui::Text("Draw Calls: %u  Triangles: %u",
                        stats.draw_calls, stats.rendered_triangles);
        }

        // Environment section
        ImGui::Separator();
        if (ImGui::CollapsingHeader("Environment", ImGuiTreeNodeFlags_DefaultOpen)) {
            if (ctx.env_path.empty()) {
                ImGui::TextDisabled("No HDR loaded (fallback)");
            } else {
                const auto filename = std::filesystem::path(ctx.env_path).filename().string();
                ImGui::Text("HDR: %s", filename.c_str());
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("%s", ctx.env_path.c_str());
                }
            }

#ifdef _WIN32
            ImGui::SameLine();
            if (ImGui::Button("Load HDR...")) {
                auto path = open_file_dialog(
                    L"HDR Files (*.hdr)\0*.hdr\0All Files (*.*)\0*.*\0",
                    L"Load HDR Environment");
                if (!path.empty()) {
                    actions.env_load_requested = true;
                    actions.new_env_path = std::move(path);
                }
            }
#endif
        }

        // Lighting section
        ImGui::Separator();
        if (ImGui::CollapsingHeader("Lighting", ImGuiTreeNodeFlags_DefaultOpen)) {
            // Light source mode combo
            constexpr const char *kModeLabels[] = {"Scene", "Fallback", "HDR Sun", "None"};
            auto mode_index = static_cast<int>(ctx.light_source_mode);
            if (ImGui::Combo("Light Source", &mode_index, kModeLabels, 4)) {
                // Prevent selecting Scene when scene has no lights
                if (mode_index == 0 && !ctx.scene_has_lights) {
                    mode_index = static_cast<int>(ctx.light_source_mode);
                }
                ctx.light_source_mode = static_cast<LightSourceMode>(mode_index);
            }
            if (!ctx.scene_has_lights && ctx.light_source_mode == LightSourceMode::Scene) {
                ctx.light_source_mode = LightSourceMode::Fallback;
            }

            // Direction display (always visible)
            ImGui::Text("Direction: Yaw %.1f%s  Pitch %.1f%s",
                        ctx.light_yaw_deg, "\xC2\xB0",
                        ctx.light_pitch_deg, "\xC2\xB0");

            ImGui::Text("Active Lights: %u", ctx.active_light_count);
            ImGui::Text("IBL Rotation: %.1f%s", ctx.ibl_rotation_deg, "\xC2\xB0");

            // Fallback light controls (only in Fallback mode)
            if (ctx.light_source_mode == LightSourceMode::Fallback) {
                ImGui::SliderFloat("Intensity", &ctx.fallback_intensity, 0.0f, 20.0f, "%.1f");
                ImGui::SliderFloat("Color Temp (K)##fallback", &ctx.fallback_color_temp,
                                   2000.0f, 12000.0f, "%.0f");
                ImGui::Checkbox("Cast Shadows", &ctx.fallback_cast_shadows);
                ImGui::TextDisabled("Alt + Left Drag to rotate");
            }

            // HDR Sun light controls (only in HdrSun mode)
            if (ctx.light_source_mode == LightSourceMode::HdrSun) {
                if (ImGui::InputInt("Sun X", &ctx.hdr_sun_x)) {
                    ctx.hdr_sun_x = std::clamp(ctx.hdr_sun_x, 0,
                        ctx.equirect_width > 0 ? static_cast<int>(ctx.equirect_width) - 1 : 0);
                    actions.hdr_sun_coords_changed = true;
                }
                if (ImGui::InputInt("Sun Y", &ctx.hdr_sun_y)) {
                    ctx.hdr_sun_y = std::clamp(ctx.hdr_sun_y, 0,
                        ctx.equirect_height > 0 ? static_cast<int>(ctx.equirect_height) - 1 : 0);
                    actions.hdr_sun_coords_changed = true;
                }
                ImGui::SliderFloat("Intensity##hdrsun", &ctx.hdr_sun_intensity, 0.0f, 20.0f, "%.1f");
                ImGui::SliderFloat("Color Temp (K)##hdrsun", &ctx.hdr_sun_color_temp,
                                   2000.0f, 12000.0f, "%.0f");
                ImGui::Checkbox("Cast Shadows##hdrsun", &ctx.hdr_sun_cast_shadows);
            }
        }

        // Features section
        ImGui::Separator();
        if (ImGui::CollapsingHeader("Features", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Checkbox("Skybox", &ctx.features.skybox);
            ImGui::Checkbox("Shadows", &ctx.features.shadows);
            ImGui::Checkbox("AO", &ctx.features.ao);
            ImGui::Checkbox("Contact Shadows##feature", &ctx.features.contact_shadows);
        }

        // Shadow section
        if (ctx.features.shadows) {
            ImGui::Separator();
            if (ImGui::CollapsingHeader("Shadow", ImGuiTreeNodeFlags_DefaultOpen)) {
                // Shadow Mode (PCF / PCSS)
                constexpr const char *kModeLabels[] = {"PCF", "PCSS"};
                auto mode_idx = static_cast<int>(ctx.shadow_config.shadow_mode);
                if (ImGui::Combo("Shadow Mode", &mode_idx, kModeLabels, IM_ARRAYSIZE(kModeLabels))) {
                    ctx.shadow_config.shadow_mode = static_cast<uint32_t>(mode_idx);
                }

                // Cascade count (pure rendering parameter, no resource rebuild)
                constexpr uint32_t kCascadeCounts[] = {1, 2, 3, 4};
                constexpr const char *kCascadeLabels[] = {"1", "2", "3", "4"};
                int cascade_idx = 0;
                for (int i = 0; i < IM_ARRAYSIZE(kCascadeCounts); ++i) {
                    if (kCascadeCounts[i] == ctx.shadow_config.cascade_count) {
                        cascade_idx = i;
                        break;
                    }
                }
                if (ImGui::Combo("Cascades", &cascade_idx, kCascadeLabels, IM_ARRAYSIZE(kCascadeLabels))) {
                    ctx.shadow_config.cascade_count = kCascadeCounts[cascade_idx];
                }

                // Resolution (triggers resource rebuild via DebugUIActions)
                constexpr uint32_t kResolutions[] = {512, 1024, 2048, 4096};
                constexpr const char *kResLabels[] = {"512", "1024", "2048", "4096"};
                int res_idx = 0;
                for (int i = 0; i < IM_ARRAYSIZE(kResolutions); ++i) {
                    if (kResolutions[i] == ctx.shadow_resolution) {
                        res_idx = i;
                        break;
                    }
                }
                if (ImGui::Combo("Resolution", &res_idx, kResLabels, IM_ARRAYSIZE(kResLabels))) {
                    actions.shadow_resolution_changed = true;
                    actions.new_shadow_resolution = kResolutions[res_idx];
                }

                ImGui::SliderFloat("Split Lambda", &ctx.shadow_config.split_lambda, 0.0f, 1.0f, "%.2f");
                ImGui::SliderFloat("Max Distance", &ctx.shadow_config.max_distance, 1.0f, 2000.0f, "%.0f m",
                                   ImGuiSliderFlags_Logarithmic);
                ImGui::SliderFloat("Slope Bias", &ctx.shadow_config.slope_bias, 0.0f, 10.0f, "%.1f");
                ImGui::SliderFloat("Normal Offset", &ctx.shadow_config.normal_offset, 0.0f, 5.0f, "%.2f");

                if (ctx.shadow_config.shadow_mode == 0) {
                    // PCF mode: show PCF radius
                    constexpr uint32_t kPcfRadii[] = {0, 1, 2, 3, 4, 5};
                    constexpr const char *kPcfLabels[] = {"Off", "3x3", "5x5", "7x7", "9x9", "11x11"};
                    int pcf_idx = 0;
                    for (int i = 0; i < IM_ARRAYSIZE(kPcfRadii); ++i) {
                        if (kPcfRadii[i] == ctx.shadow_config.pcf_radius) {
                            pcf_idx = i;
                            break;
                        }
                    }
                    if (ImGui::Combo("PCF Radius", &pcf_idx, kPcfLabels, IM_ARRAYSIZE(kPcfLabels))) {
                        ctx.shadow_config.pcf_radius = kPcfRadii[pcf_idx];
                    }
                } else {
                    // PCSS mode: show angular diameter, quality, blocker early-out
                    constexpr float kDegToRad = 3.14159265f / 180.0f;
                    constexpr float kRadToDeg = 180.0f / 3.14159265f;
                    float angular_deg = ctx.shadow_config.light_angular_diameter * kRadToDeg;
                    if (ImGui::SliderFloat("Angular Diameter", &angular_deg, 0.1f, 5.0f, "%.2f deg")) {
                        ctx.shadow_config.light_angular_diameter = angular_deg * kDegToRad;
                    }
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("Sun ~0.53 deg (subtle). Use 2-5 deg for visible soft shadows.");
                    }

                    constexpr const char *kQualityLabels[] = {"Low", "Medium", "High"};
                    auto quality_idx = static_cast<int>(ctx.shadow_config.pcss_quality);
                    if (ImGui::Combo("PCSS Quality", &quality_idx, kQualityLabels, IM_ARRAYSIZE(kQualityLabels))) {
                        ctx.shadow_config.pcss_quality = static_cast<uint32_t>(quality_idx);
                    }

                    bool early_out = (ctx.shadow_config.pcss_flags & 1u) != 0;
                    if (ImGui::Checkbox("Blocker Early-Out", &early_out)) {
                        ctx.shadow_config.pcss_flags = early_out ? (ctx.shadow_config.pcss_flags | 1u)
                                                                 : (ctx.shadow_config.pcss_flags & ~1u);
                    }
                }

                // Blend Width drives both cascade-to-cascade blend and distance fade —
                // semantically both are "transition smoothness". ShadowConfig keeps
                // them as separate fields for potential M2+ independent tuning.
                ImGui::SliderFloat("Blend Width", &ctx.shadow_config.blend_width, 0.0f, 0.5f, "%.2f");
                ctx.shadow_config.distance_fade_width = ctx.shadow_config.blend_width;

                // Cascade statistics: coverage range and texel density
                ImGui::Separator();
                ImGui::TextDisabled("Cascade Statistics");
                const auto &sc = ctx.shadow_config;
                const float shadow_far = (std::min)(sc.max_distance, ctx.camera.far_plane);
                const float tan_half = std::tan(ctx.camera.fov * 0.5f);
                // Frustum diagonal factor at unit depth
                const float diag_factor = 2.0f * tan_half
                    * std::sqrt(ctx.camera.aspect * ctx.camera.aspect + 1.0f);
                const float res_f = static_cast<float>(ctx.shadow_resolution);

                float prev_split = ctx.camera.near_plane;
                for (uint32_t i = 0; i < sc.cascade_count; ++i) {
                    const float t = static_cast<float>(i + 1)
                        / static_cast<float>(sc.cascade_count);
                    const float c_log = ctx.camera.near_plane
                        * std::pow(shadow_far / ctx.camera.near_plane, t);
                    const float c_lin = ctx.camera.near_plane
                        + (shadow_far - ctx.camera.near_plane) * t;
                    const float split = sc.split_lambda * c_log
                        + (1.0f - sc.split_lambda) * c_lin;

                    const float diagonal = diag_factor * split;
                    const float density = res_f / diagonal;

                    ImGui::Text("  C%u: %.1f - %.1f m  (%.1f px/m)",
                                i, prev_split, split, density);
                    prev_split = split;
                }
            }
        }

        // AO section
        if (ctx.features.ao) {
            ImGui::Separator();
            if (ImGui::CollapsingHeader("Ambient Occlusion", ImGuiTreeNodeFlags_DefaultOpen)) {
                ImGui::SliderFloat("Radius", &ctx.ao_config.radius, 0.1f, 5.0f, "%.2f m");

                // Directions combo (2/4/8)
                constexpr uint32_t kDirections[] = {2, 4, 8};
                constexpr const char *kDirLabels[] = {"2", "4", "8"};
                int dir_idx = 1; // default to 4
                for (int i = 0; i < 3; ++i) {
                    if (kDirections[i] == ctx.ao_config.directions) { dir_idx = i; break; }
                }
                if (ImGui::Combo("Directions", &dir_idx, kDirLabels, 3)) {
                    ctx.ao_config.directions = kDirections[dir_idx];
                }

                // Steps per direction combo (2/4/8)
                constexpr uint32_t kSteps[] = {2, 4, 8};
                constexpr const char *kStepLabels[] = {"2", "4", "8"};
                int step_idx = 1; // default to 4
                for (int i = 0; i < 3; ++i) {
                    if (kSteps[i] == ctx.ao_config.steps_per_dir) { step_idx = i; break; }
                }
                if (ImGui::Combo("Steps/Dir", &step_idx, kStepLabels, 3)) {
                    ctx.ao_config.steps_per_dir = kSteps[step_idx];
                }

                ImGui::SliderFloat("Thin Compensation", &ctx.ao_config.thin_compensation, 0.0f, 0.7f, "%.2f");
                ImGui::SliderFloat("Intensity##ao", &ctx.ao_config.intensity, 0.5f, 3.0f, "%.2f");
                ImGui::SliderFloat("Temporal Blend", &ctx.ao_config.temporal_blend, 0.0f, 0.98f, "%.2f");
                ImGui::Checkbox("GTSO (Bent Normal)", &ctx.ao_config.use_gtso);
            }
        }

        // Contact Shadows section
        if (ctx.features.contact_shadows) {
            ImGui::Separator();
            if (ImGui::CollapsingHeader("Contact Shadows", ImGuiTreeNodeFlags_DefaultOpen)) {
                // Step count combo (8/16/24/32)
                constexpr uint32_t kStepCounts[] = {8, 16, 24, 32};
                constexpr const char *kStepLabels[] = {"8", "16", "24", "32"};
                int step_idx = 1; // default to 16
                for (int i = 0; i < IM_ARRAYSIZE(kStepCounts); ++i) {
                    if (kStepCounts[i] == ctx.contact_shadow_config.step_count) {
                        step_idx = i;
                        break;
                    }
                }
                if (ImGui::Combo("Step Count", &step_idx, kStepLabels, IM_ARRAYSIZE(kStepLabels))) {
                    ctx.contact_shadow_config.step_count = kStepCounts[step_idx];
                }

                ImGui::SliderFloat("Max Distance##cs", &ctx.contact_shadow_config.max_distance,
                                   0.1f, 5.0f, "%.2f m");
                ImGui::SliderFloat("Base Thickness", &ctx.contact_shadow_config.base_thickness,
                                   0.0001f, 0.05f, "%.4f",
                                   ImGuiSliderFlags_Logarithmic);
            }
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

            if (ImGui::Button("Reload Shaders")) {
                actions.reload_shaders = true;
            }

            slider_float_deferred("IBL Intensity", &ctx.ibl_intensity, 0.0f, 5.0f, "%.2f");
            slider_float_deferred("EV", &ctx.ev, -4.0f, 4.0f, "%.1f");

            // Debug render mode
            constexpr const char *kModeLabels[] = {
                "Full PBR", "Diffuse Only", "Specular Only", "IBL Only",
                "Normal", "Metallic", "Roughness", "AO", "Shadow Cascades", "SSAO",
            };
            auto mode = static_cast<int>(ctx.debug_render_mode);
            if (ImGui::Combo("Debug View", &mode, kModeLabels, IM_ARRAYSIZE(kModeLabels))) {
                ctx.debug_render_mode = static_cast<uint32_t>(mode);
            }
        }

        // Cache section
        ImGui::Separator();
        if (ImGui::CollapsingHeader("Cache")) {
            if (ImGui::Button("Clear Texture Cache")) {
                framework::clear_cache("textures");
            }
            if (ImGui::Button("Clear IBL Cache")) {
                framework::clear_cache("ibl");
            }
            ImGui::Spacing();
            if (ImGui::Button("Clear All Cache")) {
                framework::clear_all_cache();
            }
        }

        ImGui::End();

        return actions;
    }
} // namespace himalaya::app
