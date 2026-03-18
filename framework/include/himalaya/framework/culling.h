#pragma once

/**
 * @file culling.h
 * @brief Pure geometric frustum culling: AABB vs 6 frustum planes.
 */

#include <himalaya/framework/scene_data.h>

#include <glm/mat4x4.hpp>
#include <glm/vec4.hpp>

#include <array>
#include <span>
#include <vector>

namespace himalaya::framework {
    /**
     * @brief 6-plane frustum extracted from a view-projection matrix.
     *
     * Each plane is stored as (a, b, c, d) where ax+by+cz+d >= 0 means
     * the point is inside the frustum. Normals (a,b,c) point inward and
     * are normalized (unit length).
     *
     * Works with both perspective and orthographic projections.
     */
    struct Frustum {
        /** @brief Frustum planes: left, right, bottom, top, near, far. */
        std::array<glm::vec4, 6> planes;
    };

    /**
     * @brief Extracts a normalized 6-plane frustum from a VP matrix.
     *
     * Uses the Gribb-Hartmann method. Assumes Vulkan clip space (z in [0, w]).
     *
     * @param view_projection  Combined view-projection matrix.
     * @return Frustum with 6 normalized inward-facing planes.
     */
    Frustum extract_frustum(const glm::mat4 &view_projection);

    /**
     * @brief Tests mesh instances against a frustum, outputs visible indices.
     *
     * Pure geometric culling: tests each instance's world-space AABB against
     * the 6 frustum planes using the p-vertex approach. No material classification
     * or sorting — callers handle bucketing according to their own criteria.
     *
     * @param instances    All mesh instances to test.
     * @param frustum      Frustum to cull against.
     * @param out_visible  Caller-owned buffer; cleared then filled with indices
     *                     of visible instances. Reuse across frames for zero
     *                     allocation after the first frame.
     */
    void cull_against_frustum(
        std::span<const MeshInstance> instances,
        const Frustum &frustum,
        std::vector<uint32_t> &out_visible);
} // namespace himalaya::framework
