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
    struct Camera;
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

        /** @brief Indirect light intensity multiplier. */
        float indirect_intensity;

        /** @brief Exposure value (linear scale, from pow(2, EV)). */
        float exposure;

        /** @brief IBL rotation sin(yaw) for environment horizontal rotation. */
        float ibl_rotation_sin;

        /** @brief IBL rotation cos(yaw) for environment horizontal rotation. */
        float ibl_rotation_cos;

        /** @brief Path tracing configuration parameters. */
        const framework::PTConfig &pt_config;
    };

    /**
     * @brief Rendering subsystem owning render passes, GPU buffers, and shared resources.
     *
     * Translates per-frame RenderInput into GPU data (UBO/SSBO),
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
         * @brief Destroys RT scene data (BLAS/TLAS + emissive light SSBOs).
         *
         * Called when scene resources are destroyed but build_scene_rt() will
         * not be called (e.g. scene load failure). Prevents stale acceleration
         * structures from referencing freed VB/IB device addresses.
         * No-op if RT is not supported or no RT data exists.
         */
        void destroy_scene_rt();

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

        /** @brief Returns the default sampler (linear filter, repeat wrap, linear mip). */
        [[nodiscard]] rhi::SamplerHandle default_sampler() const;

        /** @brief Returns the default 1x1 textures (white, flat normal, black). */
        [[nodiscard]] const framework::DefaultTextures &default_textures() const;

        /** @brief Returns the material system for SSBO management. */
        framework::MaterialSystem &material_system();

        /** @brief Returns the IBL module (read-only, for equirect dimensions etc.). */
        [[nodiscard]] const framework::IBL &ibl() const;

        /** @brief Returns timeline semaphore signal info for the current denoise frame (null if none). */
        [[nodiscard]] framework::Denoiser::SemaphoreSignal pending_denoise_signal() const;

        /** @brief Aborts any in-progress denoise and resets accumulation generation. */
        void abort_denoise();

        /** @brief Resets PT accumulation (called from UI Reset button). */
        void request_pt_reset();

        /** @brief Requests a manual denoise trigger (consumed in next render_path_tracing). */
        void request_manual_denoise();

        // --- Denoiser parameter accessors (for DebugUIContext binding) ---

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

        // --- Owned rendering resources ---

        /** @brief Render graph for pass orchestration and automatic barriers. */
        framework::RenderGraph render_graph_{};

        /** @brief Shader compiler with persistent disk cache. */
        framework::CachedShaderCompiler shader_compiler_{};

        /** @brief Material SSBO management (Set 0, Binding 1). */
        framework::MaterialSystem material_system_{};

        /** @brief IBL precomputation module (cubemaps, BRDF LUT, bindless registration). */
        framework::IBL ibl_{};

        /** @brief PT reference view pass (RT pipeline dispatch + accumulation). */
        passes::ReferenceViewPass reference_view_pass_{};

        /** @brief Tonemapping pass (reads HDR color, writes swapchain). */
        passes::TonemappingPass tonemapping_pass_{};

        /** @brief Acceleration structure manager (RT, initialized when rt_supported). */
        rhi::AccelerationStructureManager as_manager_{};

        /** @brief Scene acceleration structure builder (RT, builds BLAS/TLAS/GeometryInfo). */
        framework::SceneASBuilder scene_as_builder_{};

        /** @brief Emissive face light builder (RT, builds emissive triangle + alias table SSBOs). */
        framework::EmissiveLightBuilder emissive_light_builder_{};

        /** @brief PT accumulation buffer (RGBA32F, Relative 1.0x, Storage); created when rt_supported. */
        framework::RGManagedHandle managed_pt_accumulation_;

        /** @brief PT OIDN auxiliary albedo (R16G16B16A16Sfloat, Relative 1.0x, Storage | TransferSrc); created when rt_supported. */
        framework::RGManagedHandle managed_pt_aux_albedo_;

        /** @brief PT OIDN auxiliary normal (R16G16B16A16Sfloat, Relative 1.0x, Storage | TransferSrc); created when rt_supported. */
        framework::RGManagedHandle managed_pt_aux_normal_;

        /** @brief Denoised output buffer (RGBA32F, Relative 1.0x, TransferDst | Sampled); created when rt_supported. */
        framework::RGManagedHandle managed_denoised_;

        /** @brief OIDN asynchronous denoiser instance (reference view). */
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

        /** @brief Cached emissive NEE from previous PT frame (change detection → reset). */
        bool prev_emissive_nee_ = true;

        /** @brief Cached LOD max level from previous PT frame (change detection → reset). */
        uint32_t prev_lod_max_level_ = 4;

        /** @brief Cached indirect intensity from previous PT frame (change detection → reset). */
        float prev_indirect_intensity_ = 1.0f;

        /** @brief Default sampler (linear filter, repeat wrap, linear mip). */
        rhi::SamplerHandle default_sampler_;

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

        /** @brief Registered ImageHandles for swapchain images (one per swapchain image). */
        std::vector<rhi::ImageHandle> swapchain_image_handles_;

        /** @brief Monotonically increasing frame counter for temporal noise variation. */
        uint32_t frame_counter_ = 0;

        /** @brief Cached VP matrix from the previous PT frame (accumulation reset detection). */
        glm::mat4 prev_pt_view_projection_{1.0f};

        /** @brief Cached IBL rotation sin from the previous PT frame (accumulation reset detection). */
        float prev_pt_ibl_rotation_sin_ = 0.0f;

        /** @brief Cached IBL rotation cos from the previous PT frame (accumulation reset detection). */
        float prev_pt_ibl_rotation_cos_ = 1.0f;

        // --- Private helpers ---

        /**
         * @brief Resets PT accumulation state: sample count, generation, denoise bookkeeping.
         *
         * Called on camera move, IBL rotation change, resize, environment reload,
         * and denoise abort. Centralizes the 4-field reset pattern.
         */
        void reset_pt_accumulation();

        /**
         * @brief Fills GlobalUBO for the current frame.
         */
        void fill_common_gpu_data(const RenderInput &input) const;

        /**
         * @brief Path tracing render path: Reference View Pass + Tonemapping + ImGui.
         */
        void render_path_tracing(rhi::CommandBuffer &cmd, const RenderInput &input);

        /**
         * @brief Fallback render path: ImGui only (no scene / no RT).
         *
         * Clears swapchain to black, renders ImGui overlay, ensures
         * swapchain image transitions to present layout.
         */
        void render_imgui_only(rhi::CommandBuffer &cmd, const RenderInput &input);

        /** @brief Registers all swapchain images as external images in ResourceManager. */
        void register_swapchain_images();

        /** @brief Unregisters all swapchain images from ResourceManager. */
        void unregister_swapchain_images();
    };
} // namespace himalaya::app
