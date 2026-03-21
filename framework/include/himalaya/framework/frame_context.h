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

#include <array>
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

        /** @brief Shadow map (2D Array, D32Sfloat); invalid when shadows disabled. */
        RGResourceId shadow_map;

        /** @brief Previous frame's resolved depth (temporal history, D32Sfloat). */
        RGResourceId depth_prev;

        /** @brief GTAO raw output (RG8: R=diffuse AO, G=specular occlusion). */
        RGResourceId ao_noisy;

        /** @brief AO temporal-filtered output (RG8 temporal, Set 2 binding 3). */
        RGResourceId ao_filtered;

        /** @brief Contact shadow mask (R8, Set 2 binding 4). */
        RGResourceId contact_shadow_mask;

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

        // ---- Shadow draw groups (per-cascade, frustum-culled) ----

        /** @brief Per-cascade shadow opaque draw groups (frustum-culled per cascade). */
        std::array<std::span<const MeshDrawGroup>, kMaxShadowCascades> shadow_cascade_opaque_groups{};

        /** @brief Per-cascade shadow mask draw groups (frustum-culled per cascade). */
        std::array<std::span<const MeshDrawGroup>, kMaxShadowCascades> shadow_cascade_mask_groups{};

        // ---- Render configuration (non-owning references) ----

        /** @brief Runtime feature toggles (skybox, shadows, etc.). */
        const RenderFeatures *features = nullptr;

        /** @brief Shadow system parameters. */
        const ShadowConfig *shadow_config = nullptr;

        /** @brief AO configuration parameters. */
        const AOConfig *ao_config = nullptr;

        /** @brief Contact Shadows configuration parameters. */
        const ContactShadowConfig *contact_shadow_config = nullptr;

        // ---- Frame parameters ----

        /** @brief Current frame-in-flight index (0 to kMaxFramesInFlight-1). */
        uint32_t frame_index = 0;

        /** @brief Current MSAA sample count (1 = no MSAA). */
        uint32_t sample_count = 1;
    };
} // namespace himalaya::framework
