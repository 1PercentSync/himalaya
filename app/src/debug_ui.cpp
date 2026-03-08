/**
 * @file debug_ui.cpp
 * @brief DebugUI implementation: frame stats computation and ImGui panel drawing.
 */

#include <himalaya/app/debug_ui.h>

#include <himalaya/rhi/context.h>
#include <himalaya/rhi/swapchain.h>

#include <algorithm>

#include <imgui.h>
#include <spdlog/spdlog.h>

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

        ImGui::End();

        return actions;
    }

} // namespace himalaya::app
