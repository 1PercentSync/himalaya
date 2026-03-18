/**
 * @file culling.cpp
 * @brief Pure geometric frustum culling implementation.
 *
 * Extracts frustum planes from a view-projection matrix using the
 * Gribb-Hartmann method, then tests each mesh instance's world-space
 * AABB against all 6 planes using the p-vertex approach.
 */

#include <himalaya/framework/culling.h>

#include <glm/glm.hpp>

namespace himalaya::framework {
    namespace {
        // Tests whether an AABB is completely outside a frustum plane.
        // Uses the p-vertex approach: the corner most aligned with the plane
        // normal. If this corner is outside, the entire AABB is outside.
        bool aabb_outside_plane(const AABB &aabb, const glm::vec4 &plane) {
            const glm::vec3 normal(plane);
            const glm::vec3 p = {
                normal.x >= 0.0f ? aabb.max.x : aabb.min.x,
                normal.y >= 0.0f ? aabb.max.y : aabb.min.y,
                normal.z >= 0.0f ? aabb.max.z : aabb.min.z,
            };
            return glm::dot(normal, p) + plane.w < 0.0f;
        }
    }

    Frustum extract_frustum(const glm::mat4 &vp) {
        // GLM is column-major: vp[col][row]. Extract rows for the method.
        auto row = [&](const int r) -> glm::vec4 {
            return {vp[0][r], vp[1][r], vp[2][r], vp[3][r]};
        };

        const auto r0 = row(0);
        const auto r1 = row(1);
        const auto r2 = row(2);
        const auto r3 = row(3);

        // Raw plane equations (not yet normalized). Vulkan clip space: z in [0, w].
        const std::array<glm::vec4, 6> raw = {
            r3 + r0, // Left:    w + x >= 0
            r3 - r0, // Right:   w - x >= 0
            r3 + r1, // Bottom:  w + y >= 0
            r3 - r1, // Top:     w - y >= 0
            r2,       // Near:    z >= 0
            r3 - r2,  // Far:     w - z >= 0
        };

        Frustum frustum{};
        for (int i = 0; i < 6; ++i) {
            const float inv_len = 1.0f / glm::length(glm::vec3(raw[i]));
            frustum.planes[i] = raw[i] * inv_len;
        }

        return frustum;
    }

    void cull_against_frustum(const std::span<const MeshInstance> instances,
                              const Frustum &frustum,
                              std::vector<uint32_t> &out_visible) {
        out_visible.clear();

        for (uint32_t i = 0; i < static_cast<uint32_t>(instances.size()); ++i) {
            const auto &aabb = instances[i].world_bounds;

            bool outside = false;
            for (const auto &plane : frustum.planes) {
                if (aabb_outside_plane(aabb, plane)) {
                    outside = true;
                    break;
                }
            }
            if (!outside) {
                out_visible.push_back(i);
            }
        }
    }
} // namespace himalaya::framework
