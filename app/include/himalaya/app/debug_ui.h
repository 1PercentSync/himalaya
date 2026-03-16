#pragma once

/**
 * @file debug_ui.h
 * @brief Debug UI panel: frame stats, GPU info, runtime controls.
 */

#include <cstdint>
#include <string>
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

        // --- Lighting (display + controls) ---

        /** @brief Number of directional lights active this frame (0 when disabled or scene has none). */
        uint32_t active_light_count;

        /** @brief True if the scene provides directional lights (false = checkbox grayed out). */
        bool has_scene_lights;

        /** @brief Checkbox state: disable scene directional lights (IBL only). */
        bool& disable_scene_lights;

        /** @brief IBL horizontal rotation angle in degrees (display only). */
        float ibl_rotation_deg;

        // --- Render params (controls) ---

        /** @brief Editable reference to the IBL environment light intensity. */
        float& ibl_intensity;

        /** @brief Editable reference to the exposure value in EV stops. */
        float& ev;

        /** @brief Editable reference to the debug render mode (0=Full PBR, 1-7=debug). */
        uint32_t& debug_render_mode;

        // --- MSAA (display + action) ---

        /** @brief Current MSAA sample count (display only; changes via DebugUIActions). */
        uint32_t current_sample_count;

        /** @brief Bitmask of GPU-supported MSAA sample counts (VkSampleCountFlags). */
        uint32_t supported_sample_counts;

        // --- Current file paths (display) ---

        /** @brief Current scene file path (empty = no scene loaded). */
        const std::string& scene_path;

        /** @brief Current HDR environment map path (empty = fallback). */
        const std::string& env_path;

        // --- Error display ---

        /** @brief Error message to show in UI (empty = no error). Mutable for auto-dismiss. */
        std::string& error_message;

        // --- Scene statistics (display) ---

        /** @brief Per-frame scene statistics computed after frustum culling. */
        struct SceneStats {
            uint32_t total_instances;
            uint32_t total_meshes;
            uint32_t total_materials;
            uint32_t total_textures;
            uint32_t total_vertices;
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

        /** @brief True if the MSAA sample count was changed this frame. */
        bool msaa_changed = false;

        /** @brief New MSAA sample count (valid only when msaa_changed is true). */
        uint32_t new_sample_count = 0;

        /** @brief True if the user requested loading a new scene file. */
        bool scene_load_requested = false;

        /** @brief Path selected by the user (valid only when scene_load_requested is true). */
        std::string new_scene_path;

        /** @brief True if the user requested loading a new HDR environment. */
        bool env_load_requested = false;

        /** @brief Path selected by the user (valid only when env_load_requested is true). */
        std::string new_env_path;
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
