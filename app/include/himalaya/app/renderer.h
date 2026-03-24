#pragma once

/**
 * @file renderer.h
 * @brief Rendering subsystem: pass orchestration, GPU data filling, resource ownership.
 */

#include <himalaya/framework/ibl.h>
#include <himalaya/framework/material_system.h>
#include <himalaya/framework/render_graph.h>
#include <himalaya/framework/scene_data.h>
#include <himalaya/framework/texture.h>
#include <himalaya/passes/ao_spatial_pass.h>
#include <himalaya/passes/ao_temporal_pass.h>
#include <himalaya/passes/depth_prepass.h>
#include <himalaya/passes/forward_pass.h>
#include <himalaya/passes/gtao_pass.h>
#include <himalaya/passes/skybox_pass.h>
#include <himalaya/passes/shadow_pass.h>
#include <himalaya/passes/tonemapping_pass.h>
#include <himalaya/rhi/context.h>
#include <himalaya/rhi/shader.h>

#include <array>
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

        /** @brief Shader compiler instance. */
        rhi::ShaderCompiler shader_compiler_{};

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

        // --- Private helpers ---

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
