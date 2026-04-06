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
    struct AOConfig;
    struct Camera;
    struct ContactShadowConfig;
    enum class DenoiseState : uint8_t;
    enum class RenderMode : uint8_t;
    struct RenderFeatures;
    struct ShadowConfig;
} // namespace himalaya::framework

namespace himalaya::app {
    /**
     * @brief Which directional light source is active.
     *
     * Scene, Fallback, and HdrSun are mutually exclusive. None disables
     * all directional lights (IBL only). Application auto-selects on
     * scene load: Scene if glTF has lights, otherwise Fallback.
     */
    enum class LightSourceMode : uint8_t {
        Scene,    ///< Use scene's glTF directional lights.
        Fallback, ///< Use the user-controllable fallback light.
        HdrSun,   ///< Derive direction from HDR sun pixel coordinates + IBL rotation.
        None,     ///< No directional lights (IBL only).
    };

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

        /** @brief Current light source mode (mutable — combo box changes it). */
        LightSourceMode &light_source_mode;

        /** @brief Whether the scene provides directional lights (to gray out Scene option). */
        bool scene_has_lights;

        /** @brief Number of directional lights active this frame. */
        uint32_t active_light_count;

        /** @brief Current light direction yaw in degrees (display only, any mode). */
        float light_yaw_deg;

        /** @brief Current light direction pitch in degrees (display only, any mode). */
        float light_pitch_deg;

        /** @brief IBL horizontal rotation angle in degrees (display only). */
        float ibl_rotation_deg;

        // --- Fallback light controls (mutable, shown only in Fallback mode) ---

        /** @brief Fallback light intensity multiplier. */
        float &fallback_intensity;

        /** @brief Whether the fallback light casts shadows. */
        bool &fallback_cast_shadows;

        /** @brief Fallback light color temperature in Kelvin. */
        float &fallback_color_temp;

        // --- HDR Sun light controls (mutable, shown only in HdrSun mode) ---

        /** @brief HDR sun pixel X coordinate. */
        int &hdr_sun_x;

        /** @brief HDR sun pixel Y coordinate. */
        int &hdr_sun_y;

        /** @brief HDR Sun light intensity multiplier. */
        float &hdr_sun_intensity;

        /** @brief HDR Sun light color temperature in Kelvin. */
        float &hdr_sun_color_temp;

        /** @brief Whether the HDR Sun light casts shadows. */
        bool &hdr_sun_cast_shadows;

        /** @brief Original equirect image width (for UI max bounds). */
        uint32_t equirect_width;

        /** @brief Original equirect image height (for UI max bounds). */
        uint32_t equirect_height;

        // --- Render mode ---

        /** @brief Active rendering mode (mutable — combo box changes it). */
        framework::RenderMode &render_mode;

        /** @brief Whether RT hardware is available (controls PT option visibility). */
        bool rt_supported;

        // --- Path tracing state (visible when render_mode == PathTracing) ---

        /** @brief Number of PT samples accumulated so far (read-only display). */
        uint32_t pt_sample_count;

        /** @brief Target sample count (0 = unlimited). Mutable — input field. */
        uint32_t& pt_target_samples;

        /** @brief Maximum ray bounce depth (1-32). Mutable — slider. */
        uint32_t& pt_max_bounces;

        /** @brief Firefly clamping threshold (0 = disabled). Mutable — slider. */
        float& pt_max_clamp;

        /** @brief Environment map importance sampling toggle. Mutable — checkbox. */
        bool& pt_env_sampling;

        /** @brief Time elapsed since PT accumulation started, in seconds (read-only). */
        float pt_elapsed_time;

        // --- Denoiser state (visible when render_mode == PathTracing) ---

        /** @brief Denoise feature master switch. Mutable — checkbox. */
        bool& denoise_enabled;

        /** @brief Display denoised result (true) or raw accumulation (false). Mutable — toggle. */
        bool& show_denoised;

        /** @brief Automatic denoise trigger toggle. Mutable — checkbox. */
        bool& auto_denoise;

        /** @brief Auto denoise trigger interval in samples (read-only display). */
        uint32_t auto_denoise_interval;

        /** @brief Current denoise pipeline state (read-only, for status text + button gray-out). */
        framework::DenoiseState denoise_state;

        /** @brief Sample count at which the last denoise was triggered (read-only display). */
        uint32_t last_denoise_trigger_sample_count;

        /** @brief Wall-clock duration of the last OIDN filter execution, in seconds. */
        float last_denoise_duration;

        // --- Render params (controls) ---

        /** @brief Editable reference to the IBL environment light intensity. */
        float& ibl_intensity;

        /** @brief Editable reference to the exposure value in EV stops. */
        float& ev;

        /** @brief Editable reference to the debug render mode (0=Full PBR, 1-7=debug). */
        uint32_t& debug_render_mode;

        /** @brief Runtime feature toggles (skybox, shadows, etc.). */
        framework::RenderFeatures& features;

        /** @brief Shadow bias and sampling parameters (mutable for slider controls). */
        framework::ShadowConfig& shadow_config;

        /** @brief AO parameters (mutable for slider controls). */
        framework::AOConfig& ao_config;

        /** @brief Contact shadow parameters (mutable for slider controls). */
        framework::ContactShadowConfig& contact_shadow_config;

        // --- MSAA (display + action) ---

        /** @brief Current MSAA sample count (display only; changes via DebugUIActions). */
        uint32_t current_sample_count;

        /** @brief Current shadow map resolution (display only; changes via DebugUIActions). */
        uint32_t shadow_resolution;

        /** @brief Bitmask of GPU-supported MSAA sample counts (VkSampleCountFlags). */
        uint32_t supported_sample_counts;

        // --- Current file paths (display) ---

        /** @brief Current scene file path (empty = no scene loaded). */
        const std::string& scene_path;

        /** @brief Current HDR environment map path (empty = fallback). */
        const std::string& env_path;

        // --- Error display ---

        /** @brief Error message to show in UI (empty = no error). */
        const std::string& error_message;

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

        /** @brief True if the user dismissed the error banner. */
        bool error_dismissed = false;

        /** @brief True if the user clicked the Reload Shaders button. */
        bool reload_shaders = false;

        /** @brief True if the shadow map resolution was changed this frame. */
        bool shadow_resolution_changed = false;

        /** @brief New shadow map resolution (valid only when shadow_resolution_changed is true). */
        uint32_t new_shadow_resolution = 0;

        /** @brief True if the user requested loading a new HDR environment. */
        bool env_load_requested = false;

        /** @brief Path selected by the user (valid only when env_load_requested is true). */
        std::string new_env_path;

        /** @brief True if HDR sun coordinates were modified (triggers config save). */
        bool hdr_sun_coords_changed = false;

        /** @brief True if the log level was changed via the combo box. */
        bool log_level_changed = false;

        /** @brief New spdlog level enum value (valid only when log_level_changed is true). */
        int new_log_level = 0;

        /** @brief True if the user clicked the PT Reset button (clear accumulation). */
        bool pt_reset_requested = false;

        /** @brief True if the user clicked the Denoise Now button (manual trigger). */
        bool pt_denoise_requested = false;

        /** @brief True if the auto denoise interval was changed via the input box. */
        bool denoise_interval_changed = false;

        /** @brief New auto denoise interval (valid only when denoise_interval_changed is true). */
        uint32_t new_denoise_interval = 0;
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
        DebugUIActions draw(DebugUIContext& ctx);

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
