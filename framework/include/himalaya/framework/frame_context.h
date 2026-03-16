#pragma once

/**
 * @file frame_context.h
 * @brief FrameContext: all per-frame data needed by render passes.
 *
 * Pure header — no .cpp. Renderer fills a FrameContext each frame and passes
 * it to every pass's record() method. Long-lived service references
 * (ResourceManager, DescriptorManager, etc.) are NOT stored here — passes
 * capture those in setup() and access via `this`.
 */

#include <himalaya/framework/render_graph.h>
#include <himalaya/framework/scene_data.h>
#include <himalaya/framework/material_system.h>
#include <himalaya/framework/mesh.h>

#include <cstdint>
#include <span>

namespace himalaya::framework {
    /**
     * @brief Per-frame rendering context passed to every pass's record().
     *
     * Carries RG resource IDs for the current frame, non-owning references to
     * scene data, and frame-level parameters. Constructed by Renderer each
     * frame; passes consume it read-only.
     *
     * Fields are extended as new passes and features are added across phases.
     */
    struct FrameContext {
        // ---- RG resource IDs (per-frame, from import/use_managed) ----

        /** @brief Swapchain image for final presentation. */
        RGResourceId swapchain;

        /** @brief HDR color buffer (R16G16B16A16F, 1x). */
        RGResourceId hdr_color;

        /** @brief Depth buffer (D32Sfloat). */
        RGResourceId depth;

        /** @brief MSAA color buffer; invalid when sample_count == 1. */
        RGResourceId msaa_color;

        /** @brief MSAA depth buffer; invalid when sample_count == 1. */
        RGResourceId msaa_depth;

        /** @brief MSAA normal buffer; invalid when sample_count == 1. */
        RGResourceId msaa_normal;

        /** @brief Resolved normal buffer (R10G10B10A2_UNORM). */
        RGResourceId normal;

        // ---- Scene data (non-owning references) ----

        /** @brief Loaded mesh resources. */
        std::span<const Mesh> meshes;

        /** @brief Material instances for draw routing. */
        std::span<const MaterialInstance> materials;

        /** @brief Frustum culling results (visible opaque + transparent indices). */
        const CullResult *cull_result = nullptr;

        /** @brief All mesh instances in the scene. */
        std::span<const MeshInstance> mesh_instances;

        // ---- Instancing draw groups (Renderer fills after culling) ----

        /** @brief Opaque draw groups (AlphaMode::Opaque, sorted by mesh_id). */
        std::span<const MeshDrawGroup> opaque_draw_groups;

        /** @brief Alpha-mask draw groups (AlphaMode::Mask, sorted by mesh_id). */
        std::span<const MeshDrawGroup> mask_draw_groups;

        // ---- Frame parameters ----

        /** @brief Current frame-in-flight index (0 to kMaxFramesInFlight-1). */
        uint32_t frame_index = 0;

        /** @brief Current MSAA sample count (1 = no MSAA). */
        uint32_t sample_count = 1;
    };
} // namespace himalaya::framework
