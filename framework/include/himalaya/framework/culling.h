#pragma once

/**
 * @file culling.h
 * @brief Frustum culling: AABB vs 6 frustum planes.
 */

#include <himalaya/framework/material_system.h>
#include <himalaya/framework/scene_data.h>

#include <span>

namespace himalaya::framework {
    /**
     * @brief Performs view-frustum culling on scene mesh instances.
     *
     * Extracts 6 frustum planes from the camera's view-projection matrix
     * (Gribb-Hartmann method), tests each mesh instance's world-space AABB,
     * and partitions visible instances into opaque and transparent buckets.
     * Transparent instances are sorted back-to-front by AABB center distance.
     *
     * @param scene_data  Scene data containing mesh instances and camera.
     * @param materials   Material instances for alpha mode classification.
     * @return CullResult with indices of visible opaque and transparent instances.
     */
    CullResult cull_frustum(const SceneRenderData &scene_data, std::span<const MaterialInstance> materials);
} // namespace himalaya::framework
