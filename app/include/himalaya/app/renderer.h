#pragma once

/**
 * @file renderer.h
 * @brief Rendering subsystem: pass orchestration, GPU data filling, resource ownership.
 */

#include <cstdint>
#include <span>

namespace himalaya::framework {
    struct Camera;
    struct CullResult;
    struct DirectionalLight;
    struct MaterialInstance;
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
    struct RenderInput {
        /** @brief Acquired swapchain image index for the current frame. */
        uint32_t image_index;

        /** @brief Current frame-in-flight index (0 or 1). */
        uint32_t frame_index;

        /** @brief Camera state (position, matrices). */
        const framework::Camera& camera;

        /** @brief Active directional lights for this frame. */
        std::span<const framework::DirectionalLight> lights;

        /** @brief Frustum culling result (visible opaque/transparent indices). */
        const framework::CullResult& cull_result;

        /** @brief All loaded meshes (vertex/index buffer handles and counts). */
        std::span<const framework::Mesh> meshes;

        /** @brief All loaded material instances (PBR parameters, alpha mode). */
        std::span<const framework::MaterialInstance> materials;

        /** @brief All scene mesh instances (transform, mesh_id, material_id). */
        std::span<const framework::MeshInstance> mesh_instances;

        /** @brief Ambient/IBL light intensity multiplier. */
        float ambient_intensity;

        /** @brief Exposure value (linear scale, from pow(2, EV)). */
        float exposure;
    };
} // namespace himalaya::app
