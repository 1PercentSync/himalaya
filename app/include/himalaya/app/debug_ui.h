#pragma once

/**
 * @file debug_ui.h
 * @brief Debug UI panel: frame stats, GPU info, runtime controls.
 */

#include <cstdint>
#include <vector>

namespace himalaya::rhi {
    class Context;
    class Swapchain;
} // namespace himalaya::rhi

namespace himalaya::framework {
    struct Camera;
} // namespace himalaya::framework

namespace himalaya::app {

    /**
     * @brief Data passed to DebugUI each frame.
     *
     * DebugUI receives everything it needs through this struct
     * rather than holding references to subsystems. Contains both
     * display-only values and mutable references for interactive controls.
     */
    struct DebugUIContext {
        /** @brief Frame delta time in seconds (from ImGui::GetIO().DeltaTime). */
        float delta_time;

        /** @brief Vulkan context for GPU name and VRAM queries. */
        rhi::Context& context;

        /** @brief Swapchain for resolution and VSync state. */
        rhi::Swapchain& swapchain;

        // --- Camera (display + control) ---

        /** @brief Camera state for position/orientation display and parameter sliders. */
        framework::Camera& camera;

        // --- Lighting (display values computed by Application) ---

        /** @brief Active light yaw in degrees (default or glTF). */
        float light_yaw_deg;

        /** @brief Active light pitch in degrees (default or glTF). */
        float light_pitch_deg;

        /** @brief Active light intensity (default or glTF). */
        float light_intensity;

        // --- Lighting (controls, only effective when using default light) ---

        /** @brief Editable reference to the default light intensity. */
        float& default_intensity;

        /** @brief Checkbox state: force default light even with scene lights. */
        bool& force_default_light;

        /** @brief True if the scene provides directional lights (enables force checkbox). */
        bool has_scene_lights;

        // --- Render params (controls) ---

        /** @brief Editable reference to the ambient light multiplier. */
        float& ambient_intensity;

        /** @brief Editable reference to the exposure value in EV stops. */
        float& ev;

        // --- Scene statistics (display) ---

        /** @brief Per-frame scene statistics computed after frustum culling. */
        struct SceneStats {
            uint32_t total_instances;
            uint32_t total_meshes;
            uint32_t total_materials;
            uint32_t visible_opaque;
            uint32_t visible_transparent;
            uint32_t culled;
            uint32_t draw_calls;
            uint32_t rendered_triangles;
        } scene_stats;
    };

    /**
     * @brief User actions triggered from the debug panel.
     *
     * Application inspects these after each draw() call to apply side effects.
     */
    struct DebugUIActions {
        /** @brief True if the VSync checkbox was toggled this frame. */
        bool vsync_toggled = false;
    };

    /**
     * @brief ImGui debug panel: frame statistics, GPU info, and runtime controls.
     *
     * Stateless except for FrameStats accumulation. All external data
     * is passed in via DebugUIContext; side effects are communicated
     * back via DebugUIActions.
     */
    class DebugUI {
    public:
        /**
         * @brief Draws the debug panel and returns any user-triggered actions.
         * @param ctx Per-frame data needed by the panel.
         * @return Actions triggered by the user (e.g. VSync toggle).
         */
        DebugUIActions draw(const DebugUIContext& ctx);

    private:
        /**
         * @brief Periodically computes frame time statistics.
         *
         * Accumulates per-frame delta times and every kUpdateInterval seconds
         * computes average FPS, average frame time, and 1% low metrics.
         * Between updates the displayed values remain stable (no flickering).
         */
        struct FrameStats {
            float avg_fps = 0.0f;
            float avg_frame_time_ms = 0.0f;
            float low1_fps = 0.0f;
            float low1_frame_time_ms = 0.0f;

            /** @brief Feed each frame's delta time in seconds. */
            void push(float delta_time);

        private:
            static constexpr float kUpdateInterval = 1.0f;
            std::vector<float> samples_;
            float elapsed_ = 0.0f;
            void compute();
        };

        /** @brief Frame time statistics accumulator. */
        FrameStats frame_stats_;
    };

} // namespace himalaya::app
