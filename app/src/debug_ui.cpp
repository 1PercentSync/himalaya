/**
 * @file debug_ui.cpp
 * @brief DebugUI implementation: frame stats computation and ImGui panel drawing.
 */

#include <himalaya/app/debug_ui.h>

#include <himalaya/framework/cache.h>
#include <himalaya/framework/camera.h>
#include <himalaya/framework/denoiser.h>
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
        if (n == 0) { return; }

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
        {
            // Build list of supported present modes (FIFO always available)
            const char* mode_labels[3];
            rhi::PresentMode mode_values[3];
            int count = 0;
            int current_idx = 0;

            mode_labels[count] = "FIFO";
            mode_values[count] = rhi::PresentMode::Fifo;
            if (ctx.user_present_mode == rhi::PresentMode::Fifo) { current_idx = count; }
            ++count;

            if (ctx.swapchain.mailbox_supported) {
                mode_labels[count] = "Mailbox";
                mode_values[count] = rhi::PresentMode::Mailbox;
                if (ctx.user_present_mode == rhi::PresentMode::Mailbox) { current_idx = count; }
                ++count;
            }

            if (ctx.swapchain.immediate_supported) {
                mode_labels[count] = "Immediate";
                mode_values[count] = rhi::PresentMode::Immediate;
                if (ctx.user_present_mode == rhi::PresentMode::Immediate) { current_idx = count; }
                ++count;
            }

            const bool only_fifo = (count == 1);
            if (only_fifo) { ImGui::BeginDisabled(); }

            if (ImGui::Combo("Present Mode", &current_idx, mode_labels, count)) {
                ctx.user_present_mode = mode_values[current_idx];
            }

            if (only_fifo) { ImGui::EndDisabled(); }
        }

        // Render mode toggle. Checked = PT, unchecked = GS.
        {
            ImGui::Checkbox("Path Tracing", &ctx.pt_mode);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Checked: Path Tracing\nUnchecked: Gaussian Splatting");
            }
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
            actions.log_level_changed = true;
            actions.new_log_level = current_log_level;
        }

        // Path Tracing controls (visible only when PT mode is active)
        if (ctx.pt_mode) {
            ImGui::Separator();
            if (ImGui::CollapsingHeader("Path Tracing##settings", ImGuiTreeNodeFlags_DefaultOpen)) {
                // Status line
                const bool target_reached = ctx.pt_config.target_samples > 0 &&
                                            ctx.pt_sample_count >= ctx.pt_config.target_samples;
                if (ctx.pt_config.target_samples > 0) {
                    ImGui::Text("Samples: %u / %u", ctx.pt_sample_count, ctx.pt_config.target_samples);
                } else {
                    ImGui::Text("Samples: %u", ctx.pt_sample_count);
                }
                ImGui::SameLine();
                ImGui::Text("  Time: %.3fs", static_cast<double>(ctx.pt_elapsed_time));
                if (target_reached && ctx.pt_sample_count > 0) {
                    ImGui::Text("Avg: %.3f ms/sample",
                                static_cast<double>(ctx.pt_elapsed_time) /
                                ctx.pt_sample_count * 1000.0);
                }

                // Max Bounces slider (1-32)
                auto bounces = static_cast<int>(ctx.pt_config.max_bounces);
                if (ImGui::SliderInt("Max Bounces", &bounces, 1, 32)) {
                    ctx.pt_config.max_bounces = static_cast<uint32_t>(bounces);
                }

                // Firefly Clamp slider (0 = Off)
                slider_float_deferred("Firefly Clamp", &ctx.pt_config.max_clamp,
                                      0.0f, 100.0f, "%.1f");
                if (ctx.pt_config.max_clamp == 0.0f && ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Firefly clamping disabled");
                }

                // Env Importance Sampling toggle
                ImGui::Checkbox("Env Importance Sampling", &ctx.pt_config.env_sampling);

                // Emissive NEE toggle (area light importance sampling)
                ImGui::Checkbox("Emissive NEE", &ctx.pt_config.emissive_nee);

                // LOD Max Level slider (ray cone texture LOD clamp)
                {
                    int lod = static_cast<int>(ctx.pt_config.lod_max_level);
                    if (ImGui::SliderInt("LOD Max Level", &lod, 0, 12)) {
                        ctx.pt_config.lod_max_level = static_cast<uint32_t>(lod);
                    }
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("Ray cone texture LOD upper clamp.\n0 = full resolution (debug), 4 = default.");
                    }
                }

                // Allow Tearing — override present mode to IMMEDIATE in PT
                {
                    const bool can_tear = ctx.swapchain.immediate_supported;
                    if (!can_tear) { ImGui::BeginDisabled(); }
                    ImGui::Checkbox("Allow Tearing", &ctx.pt_allow_tearing);
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip(can_tear
                            ? "Force IMMEDIATE present mode while in PT to bypass\n"
                              "driver frame rate limits (e.g. Sunshine streaming)."
                            : "IMMEDIATE present mode not supported by this surface.");
                    }
                    if (!can_tear) { ImGui::EndDisabled(); }
                }

                // Target Samples input (0 = unlimited)
                ImGui::InputScalar("Target Samples", ImGuiDataType_U32, &ctx.pt_config.target_samples);
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("0 = unlimited");
                }

                // Reset button
                if (ImGui::Button("Reset")) {
                    actions.pt_reset_requested = true;
                }
            }

            // OIDN Denoiser controls
            if (ImGui::CollapsingHeader("Denoiser (OIDN)", ImGuiTreeNodeFlags_DefaultOpen)) {
                ImGui::Checkbox("Denoise", &ctx.denoise_enabled);

                ImGui::Checkbox("Show Denoised", &ctx.show_denoised);

                ImGui::Checkbox("Auto Denoise", &ctx.auto_denoise);
                if (ctx.auto_denoise) {
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(80.0f);
                    auto interval = static_cast<int>(ctx.auto_denoise_interval);
                    if (ImGui::InputInt("##Interval", &interval, 0, 0)) {
                        if (interval < 16) { interval = 16; }
                        actions.denoise_interval_changed = true;
                        actions.new_denoise_interval = static_cast<uint32_t>(interval);
                    }
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("Denoise every N samples (min 16)");
                    }
                }

                // Denoise Now button
                const bool denoise_disabled =
                    !ctx.denoise_enabled ||
                    ctx.auto_denoise ||
                    ctx.denoise_state != framework::DenoiseState::Idle ||
                    ctx.pt_sample_count == 0 ||
                    !ctx.show_denoised;
                ImGui::BeginDisabled(denoise_disabled);
                if (ImGui::Button("Denoise Now")) {
                    actions.pt_denoise_requested = true;
                }
                ImGui::EndDisabled();

                // Last denoise info
                if (ctx.last_denoise_trigger_sample_count > 0) {
                    ImGui::Text("Last triggered at: %u samples (%.3fs)",
                                ctx.last_denoise_trigger_sample_count,
                                static_cast<double>(ctx.last_denoise_duration));
                }
            }
        } // if (ctx.pt_mode)

        // Rendering section
        ImGui::Separator();
        if (ImGui::CollapsingHeader("Rendering", ImGuiTreeNodeFlags_DefaultOpen)) {
            if (ImGui::Button("Reload Shaders")) {
                actions.reload_shaders = true;
            }

            slider_float_deferred("IBL Intensity", &ctx.indirect_intensity, 0.0f, 5.0f, "%.2f");
            slider_float_deferred("EV", &ctx.ev, -4.0f, 4.0f, "%.1f");
        }

        // Camera section
        ImGui::Separator();
        if (ImGui::CollapsingHeader("Camera")) {
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

        // Scene section (file loading + asset statistics)
        ImGui::Separator();
        if (ImGui::CollapsingHeader("Scene")) {
            // PT scene picker
            if (ctx.scene_path.empty()) {
                ImGui::TextDisabled("PT: No scene loaded");
            } else {
                const auto filename = std::filesystem::path(ctx.scene_path).filename().string();
                ImGui::Text("PT: %s", filename.c_str());
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("%s", ctx.scene_path.c_str());
                }
            }

#ifdef _WIN32
            ImGui::SameLine();
            if (ImGui::Button("Load PT...")) {
                auto path = open_file_dialog(
                    L"glTF Files (*.gltf;*.glb)\0*.gltf;*.glb\0All Files (*.*)\0*.*\0",
                    L"Load PT Scene");
                if (!path.empty()) {
                    actions.scene_load_requested = true;
                    actions.new_scene_path = std::move(path);
                }
            }
#endif

            // GS scene picker
            if (ctx.gs_scene_path.empty()) {
                ImGui::TextDisabled("GS: No scene loaded");
            } else {
                const auto filename = std::filesystem::path(ctx.gs_scene_path).filename().string();
                ImGui::Text("GS: %s", filename.c_str());
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("%s", ctx.gs_scene_path.c_str());
                }
            }

#ifdef _WIN32
            ImGui::SameLine();
            if (ImGui::Button("Load GS...")) {
                auto path = open_file_dialog(
                    L"GS Files (*.gltf;*.glb;*.ply)\0*.gltf;*.glb;*.ply\0All Files (*.*)\0*.*\0",
                    L"Load GS Scene");
                if (!path.empty()) {
                    actions.gs_scene_load_requested = true;
                    actions.new_gs_scene_path = std::move(path);
                }
            }
#endif

            ImGui::Separator();
            const auto &stats = ctx.scene_stats;
            ImGui::Text("Instances: %u  Meshes: %u", stats.total_instances, stats.total_meshes);
            ImGui::Text("Materials: %u  Textures: %u", stats.total_materials, stats.total_textures);
            ImGui::Text("Vertices: %u  Triangles: %u", stats.total_vertices, stats.total_triangles);
            ImGui::Text("GS Splats: %u", ctx.gs_splat_count);
            if (ctx.gs_has_runtime_stats) {
                ImGui::Text("GS Visible: %u", ctx.gs_stats.visible_splats);
                ImGui::Text("GS Entries: requested=%u written=%u dropped=%u",
                            ctx.gs_stats.entry_requested,
                            ctx.gs_stats.entry_written,
                            ctx.gs_stats.entry_dropped);
                ImGui::Text("GS Diagnostics: invalid=%u",
                            ctx.gs_stats.invalid_entries);
            } else {
                ImGui::TextDisabled("GS runtime stats unavailable");
            }
        }

        // Environment section
        ImGui::Separator();
        if (ImGui::CollapsingHeader("Environment")) {
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

            ImGui::Text("IBL Rotation: %.1f%s", ctx.ibl_rotation_deg, "\xC2\xB0");
            ImGui::TextDisabled("Left drag to rotate");
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
            if (ImGui::Button("Clear Shader Cache")) {
                framework::clear_cache("shader_debug");
                framework::clear_cache("shader_release");
            }
        }

        ImGui::End();

        return actions;
    }
} // namespace himalaya::app
