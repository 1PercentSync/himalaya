#pragma once

/**
 * @file probe_baker_pass.h
 * @brief ProbeBakerPass: cubemap RT dispatch for reflection probe baking (Layer 2).
 *
 * Dispatches the probe baker RT pipeline at probe_face_resolution^2 per face,
 * 6 faces per frame.  Each invocation traces one sample from the probe position
 * through a cubemap face texel via the shared trace_path() bounce loop.
 *
 * Accumulation is stored in a cubemap image (6 layers, RGBA32F).  Per-face
 * 2D views are created internally and bound via push descriptors for each of
 * the 6 per-frame dispatches.
 *
 * Set 3 push descriptor layout (4 bindings):
 *   binding 0 — accumulation face view (storage image, rgba32f, ReadWrite)
 *   binding 1 — aux albedo face view   (storage image, rgba16f, Write)
 *   binding 2 — aux normal face view   (storage image, rgba16f, Write)
 *   binding 3 — Sobol direction number SSBO (readonly)
 */

#include <himalaya/framework/frame_context.h>
#include <himalaya/framework/render_graph.h>
#include <himalaya/rhi/rt_pipeline.h>
#include <himalaya/rhi/types.h>

#include <vulkan/vulkan.h>

#include <array>
#include <cstdint>

namespace himalaya::rhi {
    class Context;
    class ResourceManager;
    class DescriptorManager;
    class ShaderCompiler;
} // namespace himalaya::rhi

namespace himalaya::passes {
    /**
     * @brief Probe baker pass — cubemap RT pipeline dispatch with per-face accumulation.
     *
     * Owns the RT pipeline, Set 3 push descriptor layout, and per-face
     * VkImageViews for the accumulation cubemap and aux image arrays.
     * Images themselves are created by Renderer and communicated via
     * set_probe_images() before each record() call.
     */
    class ProbeBakerPass {
    public:
        /** @brief Number of cubemap faces. */
        static constexpr uint32_t kFaceCount = 6;

        /**
         * @brief One-time initialization: compile RT shaders, create pipeline.
         *
         * @param ctx              Vulkan context (RT properties, device).
         * @param rm               Resource manager (buffer/image access).
         * @param dm               Descriptor manager (global set layouts).
         * @param sc               Shader compiler (RT stage support).
         * @param sobol_buffer     Sobol direction number buffer (Set 3 binding 3).
         * @param blue_noise_index Bindless index of the 128x128 blue noise texture.
         */
        void setup(rhi::Context &ctx,
                   rhi::ResourceManager &rm,
                   rhi::DescriptorManager &dm,
                   rhi::ShaderCompiler &sc,
                   rhi::BufferHandle sobol_buffer,
                   uint32_t blue_noise_index);

        /**
         * @brief Register RG resource usage and provide the 6-face RT dispatch callback.
         *
         * Caller imports probe images into the RG once and passes the resource IDs.
         * One RG pass is added containing 6 sequential dispatches (one per face),
         * each with its own push descriptor update and push constant face_index.
         *
         * @param rg               Render graph to add the pass to.
         * @param ctx              Per-frame context (frame_index for global descriptor sets).
         * @param rg_accumulation  RG resource for accumulation cubemap (ReadWrite).
         * @param rg_aux_albedo    RG resource for aux albedo array (Write).
         * @param rg_aux_normal    RG resource for aux normal array (Write).
         */
        void record(framework::RenderGraph &rg, const framework::FrameContext &ctx,
                    framework::RGResourceId rg_accumulation,
                    framework::RGResourceId rg_aux_albedo,
                    framework::RGResourceId rg_aux_normal);

        /**
         * @brief Rebuild RT pipeline by recompiling shaders from disk.
         *
         * Caller must guarantee GPU is idle.
         */
        void rebuild_pipelines();

        /** @brief Destroy pipeline and Set 3 layout. */
        void destroy();

        /** @brief Resets accumulation — next frame overwrites instead of averaging. */
        void reset_accumulation();

        /** @brief Returns the number of samples accumulated so far (shared across 6 faces). */
        [[nodiscard]] uint32_t sample_count() const;

        // ---- Probe image configuration (set by Renderer before each record) ----

        /**
         * @brief Configure the per-probe images and create per-face VkImageViews.
         *
         * Must be called before record() whenever the target probe changes.
         * Creates 18 per-face 2D views (6 accumulation + 6 aux albedo + 6 aux
         * normal) that are reused across all dispatches for this probe.
         * Destroys any previously created views.
         *
         * @param accumulation Accumulation cubemap (RGBA32F, 6 layers).
         * @param aux_albedo   OIDN auxiliary albedo array (RGBA16F, 6 layers).
         * @param aux_normal   OIDN auxiliary normal array (RGBA16F, 6 layers).
         * @param face_res     Probe face resolution in texels (square).
         */
        void set_probe_images(rhi::ImageHandle accumulation,
                              rhi::ImageHandle aux_albedo,
                              rhi::ImageHandle aux_normal,
                              uint32_t face_res);

        /**
         * @brief Destroys per-face VkImageViews without destroying the parent images.
         *
         * Called when the current probe is finalized before advancing to the next,
         * or on cancel. Safe to call when no views are active.
         */
        void destroy_face_views();

        // ---- Probe position (set by Renderer per-probe) ----

        /**
         * @brief Sets the world-space probe position for push constants.
         *
         * @param x World X coordinate.
         * @param y World Y coordinate.
         * @param z World Z coordinate.
         */
        void set_probe_position(float x, float y, float z);

        // ---- PT parameter setters (mirror LightmapBakerPass interface) ----

        /** @brief Sets max ray bounce depth (used in push constants). */
        void set_max_bounces(uint32_t v);

        /** @brief Enables/disables environment map importance sampling. */
        void set_env_sampling(bool v);

        /** @brief Sets the number of emissive triangles for NEE (0 = skip). */
        void set_emissive_light_count(uint32_t v);

    private:
        /** @brief Create (or recreate) the RT pipeline. */
        void create_pipeline();

        // ---- Service pointers ----

        rhi::Context *ctx_ = nullptr;
        rhi::ResourceManager *rm_ = nullptr;
        rhi::DescriptorManager *dm_ = nullptr;
        rhi::ShaderCompiler *sc_ = nullptr;

        // ---- External resources (non-owning) ----

        rhi::BufferHandle sobol_buffer_;
        uint32_t blue_noise_index_ = 0;

        // ---- Owned resources ----

        /** @brief Set 3 push descriptor layout (4 bindings). */
        VkDescriptorSetLayout set3_layout_ = VK_NULL_HANDLE;

        /** @brief RT pipeline (pipeline + layout + SBT). */
        rhi::RTPipeline rt_pipeline_;

        // ---- Per-probe images (set by Renderer via set_probe_images) ----

        rhi::ImageHandle accumulation_;
        rhi::ImageHandle aux_albedo_;
        rhi::ImageHandle aux_normal_;
        uint32_t face_res_ = 0;

        // ---- Per-face 2D views (created in set_probe_images, destroyed in destroy_face_views) ----

        /** @brief Per-face 2D views of accumulation cubemap (layer 0-5). */
        std::array<VkImageView, kFaceCount> accum_face_views_{};

        /** @brief Per-face 2D views of aux albedo array (layer 0-5). */
        std::array<VkImageView, kFaceCount> aux_albedo_face_views_{};

        /** @brief Per-face 2D views of aux normal array (layer 0-5). */
        std::array<VkImageView, kFaceCount> aux_normal_face_views_{};

        // ---- Probe position ----

        float probe_pos_x_ = 0.0f;
        float probe_pos_y_ = 0.0f;
        float probe_pos_z_ = 0.0f;

        // ---- PT parameters ----

        uint32_t max_bounces_ = 32;
        bool env_sampling_ = true;
        uint32_t emissive_light_count_ = 0;

        // ---- Accumulation state ----

        /** @brief Shared sample count across all 6 faces (incremented once per frame). */
        uint32_t sample_count_ = 0;

        /** @brief Monotonically increasing frame seed for temporal decorrelation. */
        uint32_t frame_seed_ = 0;
    };
} // namespace himalaya::passes
