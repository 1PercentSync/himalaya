#pragma once

/**
 * @file renderer.h
 * @brief Rendering subsystem: pass orchestration, GPU data filling, resource ownership.
 */

#include <himalaya/framework/ibl.h>
#include <himalaya/framework/material_system.h>
#include <himalaya/framework/render_graph.h>
#include <himalaya/framework/texture.h>
#include <himalaya/passes/depth_prepass.h>
#include <himalaya/passes/forward_pass.h>
#include <himalaya/passes/skybox_pass.h>
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

        // --- Accessors ---

        /** @brief Returns the current MSAA sample count (1 = no MSAA). */
        [[nodiscard]] uint32_t current_sample_count() const;

        /** @brief Returns the default sampler (linear filter, repeat wrap, linear mip). */
        [[nodiscard]] rhi::SamplerHandle default_sampler() const;

        /** @brief Returns the default 1x1 textures (white, flat normal, black). */
        [[nodiscard]] const framework::DefaultTextures &default_textures() const;

        /** @brief Returns the material system for SSBO management. */
        framework::MaterialSystem &material_system();

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

        /** @brief Skybox pass (cubemap sky rendering into hdr_color). */
        passes::SkyboxPass skybox_pass_{};

        /** @brief Tonemapping pass (reads HDR color, writes swapchain). */
        passes::TonemappingPass tonemapping_pass_{};

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

        /** @brief Current MSAA sample count (1 = no MSAA, default 4x). */
        uint32_t current_sample_count_ = 4;

        /** @brief Default sampler (linear filter, repeat wrap, linear mip). */
        rhi::SamplerHandle default_sampler_;

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

        // --- Private helpers ---

        /** @brief Updates Set 2 binding 0 with the current hdr_color backing image. */
        void update_hdr_color_descriptor() const;

        /** @brief Registers all swapchain images as external images in ResourceManager. */
        void register_swapchain_images();

        /** @brief Unregisters all swapchain images from ResourceManager. */
        void unregister_swapchain_images();
    };
} // namespace himalaya::app
