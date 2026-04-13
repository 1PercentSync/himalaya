#pragma once

/**
 * @file lightmap_baker_pass.h
 * @brief LightmapBakerPass: UV-space RT dispatch for lightmap baking (Layer 2).
 *
 * Dispatches the lightmap baker RT pipeline at lightmap resolution.  Each
 * invocation reads a precomputed position/normal map, traces one sample via
 * the shared trace_path() bounce loop, and accumulates into an RGBA32F buffer.
 *
 * Set 3 push descriptor layout (6 bindings):
 *   binding 0 — accumulation (storage image, rgba32f, ReadWrite)
 *   binding 1 — aux albedo   (storage image, rgba16f, Write)
 *   binding 2 — aux normal   (storage image, rgba16f, Write)
 *   binding 3 — Sobol direction number SSBO (readonly)
 *   binding 4 — position map (combined image sampler, nearest + clamp)
 *   binding 5 — normal map   (combined image sampler, nearest + clamp)
 */

#include <himalaya/rhi/rt_pipeline.h>
#include <himalaya/rhi/types.h>

#include <vulkan/vulkan.h>

#include <cstdint>

namespace himalaya::rhi {
    class Context;
    class ResourceManager;
    class DescriptorManager;
    class ShaderCompiler;
} // namespace himalaya::rhi

namespace himalaya::framework {
    class RenderGraph;
    struct FrameContext;
} // namespace himalaya::framework

namespace himalaya::passes {
    /**
     * @brief Lightmap baker pass — UV-space RT pipeline dispatch with accumulation.
     *
     * Owns the RT pipeline, Set 3 push descriptor layout, and a nearest-clamp
     * sampler for position/normal map reads.  Managed images (accumulation,
     * aux, position/normal maps) are created by Renderer and communicated via
     * set_baker_images() before each record() call.
     */
    class LightmapBakerPass {
    public:
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
         * @brief Register RG resource usage and provide the RT dispatch callback.
         *
         * Caller must call set_baker_images() with valid handles before each
         * record() invocation.
         *
         * @param rg  Render graph to add the pass to.
         * @param ctx Per-frame context (frame_index for global descriptor sets).
         */
        void record(framework::RenderGraph &rg, const framework::FrameContext &ctx);

        /**
         * @brief Rebuild RT pipeline by recompiling shaders from disk.
         *
         * Caller must guarantee GPU is idle.
         */
        void rebuild_pipelines();

        /** @brief Destroy pipeline, sampler, and Set 3 layout. */
        void destroy();

        /** @brief Resets accumulation — next frame overwrites instead of averaging. */
        void reset_accumulation();

        /** @brief Returns the number of samples accumulated so far. */
        [[nodiscard]] uint32_t sample_count() const;

        // ---- Baker image configuration (set by Renderer before each record) ----

        /**
         * @brief Configure the per-instance baker images and lightmap dimensions.
         *
         * Must be called before record() whenever the target instance changes.
         *
         * @param accumulation Accumulation buffer (RGBA32F, lightmap resolution).
         * @param aux_albedo   OIDN auxiliary albedo (RGBA16F, lightmap resolution).
         * @param aux_normal   OIDN auxiliary normal (RGBA16F, lightmap resolution).
         * @param position_map Position map from PosNormalMapPass (RGBA32F).
         * @param normal_map   Normal map from PosNormalMapPass (RGBA32F).
         * @param width        Lightmap width in texels.
         * @param height       Lightmap height in texels.
         */
        void set_baker_images(rhi::ImageHandle accumulation,
                              rhi::ImageHandle aux_albedo,
                              rhi::ImageHandle aux_normal,
                              rhi::ImageHandle position_map,
                              rhi::ImageHandle normal_map,
                              uint32_t width, uint32_t height);

        // ---- PT parameter setters (mirror ReferenceViewPass interface) ----

        /** @brief Sets max ray bounce depth (used in push constants). */
        void set_max_bounces(uint32_t v);

        /** @brief Enables/disables environment map importance sampling. */
        void set_env_sampling(bool v);

        /** @brief Enables/disables directional lights in PT. */
        void set_directional_lights(bool v);

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

        /** @brief Set 3 push descriptor layout (6 bindings). */
        VkDescriptorSetLayout set3_layout_ = VK_NULL_HANDLE;

        /** @brief RT pipeline (pipeline + layout + SBT). */
        rhi::RTPipeline rt_pipeline_;

        /** @brief Nearest-clamp sampler for position/normal map reads. */
        VkSampler nearest_sampler_ = VK_NULL_HANDLE;

        // ---- Per-instance baker images (set by Renderer via set_baker_images) ----

        rhi::ImageHandle accumulation_;
        rhi::ImageHandle aux_albedo_;
        rhi::ImageHandle aux_normal_;
        rhi::ImageHandle position_map_;
        rhi::ImageHandle normal_map_;
        uint32_t lightmap_width_ = 0;
        uint32_t lightmap_height_ = 0;

        // ---- PT parameters ----

        uint32_t max_bounces_ = 8;
        bool env_sampling_ = true;
        bool directional_lights_ = false;
        uint32_t emissive_light_count_ = 0;
        uint32_t lod_max_level_ = 0;

        // ---- Accumulation state ----

        uint32_t sample_count_ = 0;
        uint32_t frame_seed_ = 0;
    };
} // namespace himalaya::passes
