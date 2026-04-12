#pragma once

/**
 * @file renderer.h
 * @brief Rendering subsystem: pass orchestration, GPU data filling, resource ownership.
 */

#include <himalaya/framework/cached_shader_compiler.h>
#include <himalaya/framework/denoiser.h>
#include <himalaya/framework/ibl.h>
#include <himalaya/framework/material_system.h>
#include <himalaya/framework/render_graph.h>
#include <himalaya/framework/emissive_light_builder.h>
#include <himalaya/framework/scene_as_builder.h>
#include <himalaya/framework/scene_data.h>
#include <himalaya/framework/texture.h>
#include <himalaya/rhi/acceleration_structure.h>
#include <himalaya/passes/ao_spatial_pass.h>
#include <himalaya/passes/ao_temporal_pass.h>
#include <himalaya/passes/contact_shadows_pass.h>
#include <himalaya/passes/depth_prepass.h>
#include <himalaya/passes/forward_pass.h>
#include <himalaya/passes/gtao_pass.h>
#include <himalaya/passes/skybox_pass.h>
#include <himalaya/passes/shadow_pass.h>
#include <himalaya/passes/reference_view_pass.h>
#include <himalaya/passes/tonemapping_pass.h>
#include <himalaya/rhi/context.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <span>
#include <vector>

namespace himalaya::rhi {
    class CommandBuffer;
    class DescriptorManager;
    class Swapchain;
}

namespace himalaya::framework {
    struct AABB;
    struct Camera;
    struct CullResult;
    struct DirectionalLight;
    class ImGuiBackend;
    struct Mesh;
    struct MeshInstance;
}

namespace himalaya::app {
    /**
     * @brief Per-frame semantic data passed from Application to Renderer.
     *
     * Application fills this struct each frame with scene state and rendering
     * parameters. Renderer translates it into GPU-side data (UBO/SSBO) and
     * drives the render graph. Contains non-owning references only.
     */
    struct RenderInput { // NOLINT(*-pro-type-member-init)
        /** @brief Acquired swapchain image index for the current frame. */
        uint32_t image_index;

        /** @brief Current frame-in-flight index (0 or 1). */
        uint32_t frame_index;

        /** @brief Active rendering mode (rasterization or path tracing). */
        framework::RenderMode render_mode;

        /** @brief Camera state (position, matrices). */
        const framework::Camera &camera;

        /** @brief Active directional lights for this frame. */
        std::span<const framework::DirectionalLight> lights;

        /** @brief Frustum culling result (visible opaque/transparent indices). */
        const framework::CullResult &cull_result;

        /** @brief All loaded meshes (vertex/index buffer handles and counts). */
        std::span<const framework::Mesh> meshes;

        /** @brief All loaded material instances (PBR parameters, alpha mode). */
        std::span<const framework::MaterialInstance> materials;

        /** @brief All scene mesh instances (transform, mesh_id, material_id). */
        std::span<const framework::MeshInstance> mesh_instances;

        /** @brief IBL environment light intensity multiplier. */
        float ibl_intensity;

        /** @brief Exposure value (linear scale, from pow(2, EV)). */
        float exposure;

        /** @brief IBL rotation sin(yaw) for environment horizontal rotation. */
        float ibl_rotation_sin;

        /** @brief IBL rotation cos(yaw) for environment horizontal rotation. */
        float ibl_rotation_cos;

        /** @brief Debug render mode (0=Full PBR, 1-7=debug visualizations). */
        uint32_t debug_render_mode;

        /** @brief Runtime feature toggles (skybox, shadows, etc.). */
        const framework::RenderFeatures &features;

        /** @brief Shadow system parameters. */
        const framework::ShadowConfig &shadow_config;

        /** @brief AO configuration parameters. */
        const framework::AOConfig &ao_config;

        /** @brief Contact Shadows configuration parameters. */
        const framework::ContactShadowConfig &contact_shadow_config;

        /** @brief Scene world-space AABB for shadow Z range extension. */
        const framework::AABB &scene_bounds;
    };

    /**
     * @brief Rendering subsystem owning render passes, GPU buffers, and shared resources.
     *
     * Extracted from Application to separate rendering concerns from window/input
     * management. Translates per-frame RenderInput into GPU data (UBO/SSBO),
     * builds and executes the render graph.
     *
     * Lifetime: init() after RHI infrastructure is ready, destroy() before RHI teardown.
     * Resize: on_swapchain_invalidated() before swapchain recreate,
     *         on_swapchain_recreated() after.
     */
    class Renderer {
    public:
        /**
         * @brief Initializes rendering resources: pipelines, buffers, default textures.
         *
         * Must be called after RHI context, swapchain, resource manager, and
         * descriptor manager are initialized. Stores non-owning references to
         * all subsystems for later use.
         */
        void init(rhi::Context &ctx, rhi::Swapchain &swapchain,
                  rhi::ResourceManager &rm, rhi::DescriptorManager &dm,
                  framework::ImGuiBackend &imgui,
                  const std::string &hdr_path);

        /**
         * @brief Fills GPU buffers and executes all render passes for one frame.
         * @param cmd Command buffer to record into (already begun by Application).
         * @param input Per-frame semantic data from Application.
         */
        void render(rhi::CommandBuffer &cmd, const RenderInput &input);

        /**
         * @brief Pre-resize cleanup: unregisters swapchain images and destroys
         *        resolution-dependent resources before swapchain recreation.
         */
        void on_swapchain_invalidated();

        /**
         * @brief Post-resize rebuild: re-registers swapchain images and recreates
         *        resolution-dependent resources after swapchain recreation.
         */
        void on_swapchain_recreated();

        /** @brief Destroys all owned rendering resources in reverse init order. */
        void destroy();

        /**
         * @brief Handles MSAA sample count change: waits for GPU idle, then
         *        creates/destroys/updates MSAA managed resources and rebuilds
         *        affected pipelines.
         *
         * Safe to call between begin_frame() and render(). No-op if
         * new_sample_count equals current_sample_count_.
         *
         * @param new_sample_count New MSAA sample count (1/2/4/8).
         */
        void handle_msaa_change(uint32_t new_sample_count);

        /**
         * @brief Handles shadow map resolution change: waits for GPU idle, then
         *        destroys and recreates shadow map resources at the new resolution
         *        and updates the Set 2 shadow map descriptor.
         *
         * No-op if new_resolution equals the current shadow map resolution.
         *
         * @param new_resolution New shadow map resolution (512/1024/2048/4096).
         */
        void handle_shadow_resolution_changed(uint32_t new_resolution);

        /**
         * @brief Builds scene RT data: acceleration structures + emissive light tables.
         *
         * Must be called within a Context::begin_immediate() / end_immediate() scope.
         * Builds BLAS, TLAS, Geometry Info SSBO (Set 0 binding 4/5), and
         * EmissiveLightBuilder triangle + alias table SSBOs (Set 0 binding 7/8).
         * Safe to call multiple times (auto-destroys previous resources).
         * No-op if RT is not supported.
         *
         * @param meshes         All loaded meshes.
         * @param instances      All scene mesh instances.
         * @param materials      All loaded material instances.
         * @param gpu_materials  GPU material data (emissive_factor lookup).
         * @param mesh_vertices  CPU vertex data per mesh (parallel to meshes).
         * @param mesh_indices   CPU index data per mesh (parallel to meshes).
         */
        void build_scene_rt(std::span<const framework::Mesh> meshes,
                            std::span<const framework::MeshInstance> instances,
                            std::span<const framework::MaterialInstance> materials,
                            std::span<const framework::GPUMaterialData> gpu_materials,
                            std::span<const std::vector<framework::Vertex>> mesh_vertices,
                            std::span<const std::vector<uint32_t>> mesh_indices);

        /**
         * @brief Recompiles all shaders from disk and rebuilds every pipeline.
         *
         * Waits for GPU idle, then calls rebuild_pipelines() on each pass.
         */
        void reload_shaders();

        /**
         * @brief Reloads the IBL environment from a new HDR file.
         *
         * Caller must ensure GPU is idle (vkQueueWaitIdle) before calling.
         * Destroys current IBL resources and reinitializes from the new path.
         * Empty or invalid path triggers IBL fallback (gray cubemaps).
         *
         * @param hdr_path Path to the new .hdr environment map.
         * @return true if the HDR file loaded successfully, false if fallback was used.
         */
        bool reload_environment(const std::string &hdr_path);

        // --- Accessors ---

        /** @brief Returns the current MSAA sample count (1 = no MSAA). */
        [[nodiscard]] uint32_t current_sample_count() const;

        /** @brief Returns the default sampler (linear filter, repeat wrap, linear mip). */
        [[nodiscard]] rhi::SamplerHandle default_sampler() const;

        /** @brief Returns the default 1x1 textures (white, flat normal, black). */
        [[nodiscard]] const framework::DefaultTextures &default_textures() const;

        /** @brief Returns the material system for SSBO management. */
        framework::MaterialSystem &material_system();

        /** @brief Returns the total GPU draw calls (vkCmdDrawIndexed) from the last frame. */
        [[nodiscard]] uint32_t last_draw_call_count() const;

        /** @brief Returns the IBL module (read-only, for equirect dimensions etc.). */
        [[nodiscard]] const framework::IBL &ibl() const;

        /** @brief Returns the current shadow map resolution (width = height). */
        [[nodiscard]] uint32_t shadow_resolution() const;

        /** @brief Returns timeline semaphore signal info for the current denoise frame (null if none). */
        [[nodiscard]] framework::Denoiser::SemaphoreSignal pending_denoise_signal() const;

        /** @brief Aborts any in-progress denoise and resets accumulation generation. */
        void abort_denoise();

        /** @brief Resets PT accumulation (called from UI Reset button). */
        void request_pt_reset();

        /** @brief Requests a manual denoise trigger (consumed in next render_path_tracing). */
        void request_manual_denoise();

        // --- PT parameter accessors (for DebugUIContext binding) ---

        /** @brief Mutable reference to max bounce depth (default 8, range 1-32). */
        uint32_t& pt_max_bounces();

        /** @brief Mutable reference to firefly clamp threshold (default 10.0, 0 = disabled). */
        float& pt_max_clamp();

        /** @brief Mutable reference to env map importance sampling toggle. */
        bool& pt_env_sampling();

        /** @brief Mutable reference to PT directional lights toggle. */
        bool& pt_directional_lights();

        /** @brief Mutable reference to emissive NEE toggle. */
        bool& pt_emissive_nee();

        /** @brief Mutable reference to ray cone LOD max level. */
        uint32_t& pt_lod_max_level();

        /** @brief Mutable reference to target sample count (0 = unlimited). */
        uint32_t& pt_target_samples();

        /** @brief Mutable reference to denoise enabled flag. */
        bool& denoise_enabled();

        /** @brief Mutable reference to show denoised flag. */
        bool& show_denoised();

        /** @brief Mutable reference to auto denoise flag. */
        bool& auto_denoise();

        /** @brief Mutable reference to auto denoise interval. */
        uint32_t& auto_denoise_interval();

        // --- PT read-only state ---

        /** @brief Returns number of accumulated PT samples. */
        [[nodiscard]] uint32_t pt_sample_count() const;

        /** @brief Returns elapsed time since PT accumulation started, in seconds. */
        [[nodiscard]] float pt_elapsed_time() const;

        /** @brief Returns current denoise pipeline state. */
        [[nodiscard]] framework::DenoiseState denoise_state() const;

        /** @brief Returns sample count at last denoise trigger. */
        [[nodiscard]] uint32_t last_denoise_trigger_sample_count() const;

        /** @brief Returns wall-clock duration of the last OIDN filter execution, in seconds. */
        [[nodiscard]] float last_denoise_duration() const;

    private:
        // --- Subsystem references (non-owning, set during init) ---

        /** @brief Vulkan context: device, queues, frame data. */
        rhi::Context *ctx_ = nullptr;

        /** @brief Swapchain: extent, format, images. */
        rhi::Swapchain *swapchain_ = nullptr;

        /** @brief GPU resource pool. */
        rhi::ResourceManager *resource_manager_ = nullptr;

        /** @brief Descriptor set management. */
        rhi::DescriptorManager *descriptor_manager_ = nullptr;

        /** @brief ImGui integration backend. */
        framework::ImGuiBackend *imgui_ = nullptr;

        // --- Owned rendering resources (migrated from Application) ---

        /** @brief Render graph for pass orchestration and automatic barriers. */
        framework::RenderGraph render_graph_{};

        /** @brief Shader compiler with persistent disk cache. */
        framework::CachedShaderCompiler shader_compiler_{};

        /** @brief Material SSBO management (Set 0, Binding 2). */
        framework::MaterialSystem material_system_{};

        /** @brief IBL precomputation module (cubemaps, BRDF LUT, bindless registration). */
        framework::IBL ibl_{};

        /** @brief Depth + Normal PrePass (fills depth and normal buffers). */
        passes::DepthPrePass depth_prepass_{};

        /** @brief Forward lighting pass (renders to HDR color + depth). */
        passes::ForwardPass forward_pass_{};

        /** @brief CSM shadow pass (depth rendering from light space). */
        passes::ShadowPass shadow_pass_{};

        /** @brief Skybox pass (cubemap sky rendering into hdr_color). */
        passes::SkyboxPass skybox_pass_{};

        /** @brief Tonemapping pass (reads HDR color, writes swapchain). */
        passes::TonemappingPass tonemapping_pass_{};

        /** @brief GTAO compute pass (horizon search AO). */
        passes::GTAOPass gtao_pass_{};

        /** @brief AO spatial denoising compute pass (5x5 bilateral blur). */
        passes::AOSpatialPass ao_spatial_pass_{};

        /** @brief AO temporal filter compute pass (reprojection + rejection + blend). */
        passes::AOTemporalPass ao_temporal_pass_{};

        /** @brief Contact shadows compute pass (screen-space ray march). */
        passes::ContactShadowsPass contact_shadows_pass_{};

        /** @brief PT reference view pass (RT pipeline dispatch + accumulation). */
        passes::ReferenceViewPass reference_view_pass_{};

        /** @brief Acceleration structure manager (RT, initialized when rt_supported). */
        rhi::AccelerationStructureManager as_manager_{};

        /** @brief Scene acceleration structure builder (RT, builds BLAS/TLAS/GeometryInfo). */
        framework::SceneASBuilder scene_as_builder_{};

        /** @brief Emissive face light builder (RT, builds emissive triangle + alias table SSBOs). */
        framework::EmissiveLightBuilder emissive_light_builder_{};

        /** @brief HDR color buffer (R16G16B16A16F, 1x, managed, auto-rebuilt on resize). */
        framework::RGManagedHandle managed_hdr_color_;

        /** @brief Depth buffer (D32Sfloat, 1x, managed by render graph, auto-rebuilt on resize). */
        framework::RGManagedHandle managed_depth_;

        /** @brief MSAA color buffer (R16G16B16A16F, Nx, managed); invalid when sample_count == 1. */
        framework::RGManagedHandle managed_msaa_color_;

        /** @brief MSAA depth buffer (D32Sfloat, Nx, managed); invalid when sample_count == 1. */
        framework::RGManagedHandle managed_msaa_depth_;

        /** @brief MSAA normal buffer (R10G10B10A2, Nx, managed); invalid when sample_count == 1. */
        framework::RGManagedHandle managed_msaa_normal_;

        /** @brief Resolved normal buffer (R10G10B10A2, 1x, managed, auto-rebuilt on resize). */
        framework::RGManagedHandle managed_normal_;

        /** @brief GTAO raw output (RG8, non-temporal, Storage | Sampled). */
        framework::RGManagedHandle managed_ao_noisy_;

        /** @brief Spatially denoised AO (RG8, non-temporal, Storage | Sampled). */
        framework::RGManagedHandle managed_ao_blurred_;

        /** @brief AO temporal-filtered output (RG8, temporal, Storage | Sampled). */
        framework::RGManagedHandle managed_ao_filtered_;

        /** @brief Contact shadow mask (R8, non-temporal, Storage | Sampled). */
        framework::RGManagedHandle managed_contact_shadow_mask_;

        /** @brief Resolved roughness buffer (R8Unorm, 1x, ColorAttachment | Sampled). */
        framework::RGManagedHandle managed_roughness_;

        /** @brief MSAA roughness buffer (R8Unorm, Nx, ColorAttachment); invalid when sample_count == 1. */
        framework::RGManagedHandle managed_msaa_roughness_;

        /** @brief PT accumulation buffer (RGBA32F, Relative 1.0x, Storage); created when rt_supported. */
        framework::RGManagedHandle managed_pt_accumulation_;

        /** @brief PT OIDN auxiliary albedo (R16G16B16A16Sfloat, Relative 1.0x, Storage | TransferSrc); created when rt_supported. */
        framework::RGManagedHandle managed_pt_aux_albedo_;

        /** @brief PT OIDN auxiliary normal (R16G16B16A16Sfloat, Relative 1.0x, Storage | TransferSrc); created when rt_supported. */
        framework::RGManagedHandle managed_pt_aux_normal_;

        /** @brief Denoised output buffer (RGBA32F, Relative 1.0x, TransferDst | Sampled); created when rt_supported. */
        framework::RGManagedHandle managed_denoised_;

        /** @brief OIDN asynchronous denoiser instance. */
        framework::Denoiser denoiser_{};

        /** @brief Timeline semaphore signal to inject into the current frame's submit. Set by launch_processing(), cleared at frame start. */
        framework::Denoiser::SemaphoreSignal pending_semaphore_signal_{};

        /** @brief Monotonically increasing counter, incremented on every accumulation reset. */
        uint32_t accumulation_generation_ = 0;

        /** @brief Generation of the last successfully uploaded denoised result (UINT32_MAX = none). */
        uint32_t denoised_generation_ = UINT32_MAX;

        /**
         * @brief True when an upload pass was recorded but complete_upload() is deferred to next frame.
         *
         * Ensures complete_upload() is only called after the GPU has actually
         * executed the upload pass (next frame's begin_frame fence wait guarantees this).
         */
        bool upload_pending_completion_ = false;

        /** @brief Accumulation generation to assign to denoised_generation_ when completing deferred upload. */
        uint32_t pending_denoised_generation_ = 0;

        /** @brief Denoise feature toggle (master switch). */
        bool denoise_enabled_ = true;

        /** @brief Automatic denoise trigger toggle. */
        bool auto_denoise_ = true;

        /** @brief Trigger denoise every N accumulated samples. */
        uint32_t auto_denoise_interval_ = 64;

        /** @brief Sample count at which the last denoise was triggered (for interval calculation). */
        uint32_t last_denoise_trigger_sample_count_ = 0;

        /** @brief Display denoised result (true) or raw accumulation (false). */
        bool show_denoised_ = true;

        /** @brief PT max bounce depth (default 16, range 1-32, exposed via UI Step 10). */
        uint32_t max_bounces_ = 16;

        /** @brief PT firefly clamp threshold (default 0 = disabled, OIDN denoise suffices). */
        float max_clamp_ = 0.0f;

        /** @brief Environment map importance sampling toggle (default true, exposed via UI). */
        bool env_sampling_ = true;

        /** @brief Directional lights enabled in PT (default false — env sampling replaces them). */
        bool directional_lights_ = false;

        /** @brief Emissive area light NEE toggle (default true, exposed via UI). */
        bool emissive_nee_ = true;

        /** @brief Ray cone LOD upper clamp (default 4, 0 = full resolution). */
        uint32_t lod_max_level_ = 4;

        /** @brief PT target sample count (0 = unlimited, exposed via UI Step 10). */
        uint32_t target_samples_ = 2048;

        /** @brief Time point when PT accumulation (re)started, for elapsed time display. */
        std::chrono::steady_clock::time_point pt_start_time_{};

        /** @brief Time point when target samples was reached (freezes elapsed timer). Default = start (not reached). */
        std::chrono::steady_clock::time_point pt_finish_time_{};

        /** @brief Manual denoise trigger flag (set by UI, consumed in render_path_tracing). */
        bool manual_denoise_requested_ = false;

        /** @brief Cached max bounces from previous PT frame (change detection → reset). */
        uint32_t prev_max_bounces_ = 16;

        /** @brief Cached max clamp from previous PT frame (change detection → reset). */
        float prev_max_clamp_ = 0.0f;

        /** @brief Cached env sampling from previous PT frame (change detection → reset). */
        bool prev_env_sampling_ = true;

        /** @brief Cached directional lights from previous PT frame (change detection → reset). */
        bool prev_directional_lights_ = false;

        /** @brief Cached emissive NEE from previous PT frame (change detection → reset). */
        bool prev_emissive_nee_ = true;

        /** @brief Cached LOD max level from previous PT frame (change detection → reset). */
        uint32_t prev_lod_max_level_ = 4;

        /** @brief Cached IBL intensity from previous PT frame (change detection → reset). */
        float prev_ibl_intensity_ = 1.0f;

        /** @brief Current MSAA sample count (1 = no MSAA, default 4x). */
        uint32_t current_sample_count_ = 4;

        /** @brief Default sampler (linear filter, repeat wrap, linear mip). */
        rhi::SamplerHandle default_sampler_;

        /** @brief Shadow comparison sampler (GREATER_OR_EQUAL, linear filter, clamp to edge). */
        rhi::SamplerHandle shadow_comparison_sampler_;

        /** @brief Shadow depth sampler (NEAREST, no compare, clamp to edge) for PCSS blocker search. */
        rhi::SamplerHandle shadow_depth_sampler_;

        /** @brief Nearest clamp sampler (nearest filter, clamp to edge) for screen-space reads. */
        rhi::SamplerHandle nearest_clamp_sampler_;

        /** @brief Linear clamp sampler (linear filter, clamp to edge) for screen-space effect reads. */
        rhi::SamplerHandle linear_clamp_sampler_;

        /** @brief Default 1x1 textures (white, flat normal, black). */
        framework::DefaultTextures default_textures_{};

        /** @brief 128x128 R8Unorm blue noise texture for PT sampling (Cranley-Patterson rotation). */
        rhi::ImageHandle blue_noise_image_;

        /** @brief Bindless index of the blue noise texture in Set 1 textures[]. */
        rhi::BindlessIndex blue_noise_bindless_;

        /** @brief 128-dim Sobol direction number SSBO for PT quasi-random sampling (Set 3 binding 3, 16 KB). */
        rhi::BufferHandle sobol_buffer_;

        /** @brief Per-frame GlobalUBO buffers (CpuToGpu, one per frame in flight). */
        std::array<rhi::BufferHandle, rhi::kMaxFramesInFlight> global_ubo_buffers_{};

        /** @brief Per-frame LightBuffer SSBOs (CpuToGpu, one per frame in flight). */
        std::array<rhi::BufferHandle, rhi::kMaxFramesInFlight> light_buffers_{};

        /** @brief Per-frame InstanceBuffer SSBOs (CpuToGpu, one per frame in flight). */
        std::array<rhi::BufferHandle, rhi::kMaxFramesInFlight> instance_buffers_{};

        /** @brief Registered ImageHandles for swapchain images (one per swapchain image). */
        std::vector<rhi::ImageHandle> swapchain_image_handles_;

        // ---- Instancing working buffers (reused across frames, zero-alloc after first frame) ----

        /** @brief Sorted copy of visible_opaque_indices (by mesh_id). */
        std::vector<uint32_t> sorted_opaque_indices_;

        /** @brief Opaque draw groups built from sorted indices (AlphaMode::Opaque). */
        std::vector<framework::MeshDrawGroup> opaque_draw_groups_;

        /** @brief Mask draw groups built from sorted indices (AlphaMode::Mask). */
        std::vector<framework::MeshDrawGroup> mask_draw_groups_;

        // ---- Per-cascade shadow culling working buffers ----

        /** @brief Temporary buffer for per-cascade frustum cull output (reused across cascades). */
        std::vector<uint32_t> shadow_cull_buffer_;

        /** @brief Per-cascade sorted non-Blend visible indices after frustum culling. */
        std::array<std::vector<uint32_t>, framework::kMaxShadowCascades> shadow_cascade_sorted_;

        /** @brief Per-cascade shadow opaque draw groups (frustum-culled). */
        std::array<std::vector<framework::MeshDrawGroup>, framework::kMaxShadowCascades> shadow_cascade_opaque_groups_;

        /** @brief Per-cascade shadow mask draw groups (frustum-culled). */
        std::array<std::vector<framework::MeshDrawGroup>, framework::kMaxShadowCascades> shadow_cascade_mask_groups_;

        /** @brief Total vkCmdDrawIndexed calls from the last frame (across all scene passes). */
        uint32_t draw_call_count_ = 0;

        /** @brief Cached view-projection from the previous frame (temporal reprojection). */
        glm::mat4 prev_view_projection_{1.0f};

        /** @brief Monotonically increasing frame counter for temporal noise variation. */
        uint32_t frame_counter_ = 0;

        /** @brief Cached VP matrix from the previous PT frame (accumulation reset detection). */
        glm::mat4 prev_pt_view_projection_{1.0f};

        /** @brief Cached IBL rotation sin from the previous PT frame (accumulation reset detection). */
        float prev_pt_ibl_rotation_sin_ = 0.0f;

        /** @brief Cached IBL rotation cos from the previous PT frame (accumulation reset detection). */
        float prev_pt_ibl_rotation_cos_ = 1.0f;

        /** @brief Cached light count from the previous PT frame (accumulation reset detection). */
        uint32_t prev_pt_light_count_ = 0;

        /** @brief Cached light direction+intensity from the previous PT frame (accumulation reset detection). */
        glm::vec4 prev_pt_light_dir_intensity_{0.0f};

        /** @brief Cached light color+shadow from the previous PT frame (accumulation reset detection). */
        glm::vec4 prev_pt_light_color_shadow_{0.0f};

        // --- Private helpers ---

        /**
         * @brief Resets PT accumulation state: sample count, generation, denoise bookkeeping.
         *
         * Called on camera move, IBL rotation change, resize, environment reload,
         * and denoise abort. Centralizes the 4-field reset pattern.
         */
        void reset_pt_accumulation();

        /**
         * @brief Fills GlobalUBO and LightBuffer for the current frame.
         *
         * Shared by both rasterization and path tracing paths. Writes all UBO
         * fields (including shadow cascade data) and the light SSBO.
         */
        void fill_common_gpu_data(const RenderInput &input) const;

        /**
         * @brief Rasterization render path: instancing, draw groups, full multi-pass pipeline.
         */
        void render_rasterization(rhi::CommandBuffer &cmd, const RenderInput &input);

        /**
         * @brief Path tracing render path: Reference View Pass + Tonemapping + ImGui.
         */
        void render_path_tracing(rhi::CommandBuffer &cmd, const RenderInput &input);

        /** @brief Updates Set 2 binding 0 with the current hdr_color backing image. */
        void update_hdr_color_descriptor() const;

        /** @brief Updates Set 2 binding 5 with the current shadow map image (comparison sampler). */
        void update_shadow_map_descriptor() const;

        /** @brief Updates Set 2 binding 6 with the current shadow map image (depth sampler for PCSS). */
        void update_shadow_depth_descriptor() const;

        /** @brief Updates Set 2 binding 1 with the current depth_resolved backing image (nearest). */
        void update_depth_descriptor() const;

        /** @brief Updates Set 2 binding 2 with the current normal_resolved backing image (nearest). */
        void update_normal_descriptor() const;

        /** @brief Updates Set 2 binding 3 with the current ao_filtered backing image (linear). */
        void update_ao_descriptor() const;

        /** @brief Updates Set 2 binding 4 with the current contact_shadow_mask backing image (linear). */
        void update_contact_shadow_descriptor() const;

        /** @brief Registers all swapchain images as external images in ResourceManager. */
        void register_swapchain_images();

        /** @brief Unregisters all swapchain images from ResourceManager. */
        void unregister_swapchain_images();
    };
} // namespace himalaya::app
