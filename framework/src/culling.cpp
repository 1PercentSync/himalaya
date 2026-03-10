/**
 * @file culling.cpp
 * @brief Frustum culling implementation.
 *
 * Extracts frustum planes from the view-projection matrix using the
 * Gribb-Hartmann method, then tests each mesh instance's world-space
 * AABB against all 6 planes using the p-vertex approach.
 */

#include <himalaya/framework/culling.h>

#include <glm/glm.hpp>

#include <algorithm>
#include <array>

namespace himalaya::framework {
    namespace {
        // Frustum plane in Hessian normal form: normal·p + distance >= 0 means inside.
        struct Plane {
            glm::vec3 normal;
            float distance;
        };

        // Extracts 6 frustum planes from a view-projection matrix.
        // Uses the Gribb-Hartmann method. Planes point inward (positive
        // half-space is inside the frustum). Vulkan clip space: z in [0, w].
        std::array<Plane, 6> extract_frustum_planes(const glm::mat4 &vp) {
            // GLM is column-major: vp[col][row]. Extract rows for the method.
            auto row = [&](const int r) -> glm::vec4 {
                return {vp[0][r], vp[1][r], vp[2][r], vp[3][r]};
            };

            const auto r0 = row(0);
            const auto r1 = row(1);
            const auto r2 = row(2);
            const auto r3 = row(3);

            // Raw plane equations (not yet normalized)
            const std::array<glm::vec4, 6> raw = {
                r3 + r0, // Left:    w + x >= 0
                r3 - r0, // Right:   w - x >= 0
                r3 + r1, // Bottom:  w + y >= 0
                r3 - r1, // Top:     w - y >= 0
                r2, // Near:    z >= 0     (Vulkan [0, w])
                r3 - r2, // Far:     w - z >= 0
            };

            std::array<Plane, 6> planes{};
            for (int i = 0; i < 6; ++i) {
                const float len = glm::length(glm::vec3(raw[i]));
                const float inv_len = 1.0f / len;
                planes[i].normal = glm::vec3(raw[i]) * inv_len;
                planes[i].distance = raw[i].w * inv_len;
            }

            return planes;
        }

        // Tests whether an AABB is completely outside a frustum plane.
        // Uses the p-vertex approach: the corner most aligned with the plane
        // normal. If this corner is outside, the entire AABB is outside.
        bool aabb_outside_plane(const AABB &aabb, const Plane &plane) {
            const glm::vec3 p = {
                plane.normal.x >= 0.0f ? aabb.max.x : aabb.min.x,
                plane.normal.y >= 0.0f ? aabb.max.y : aabb.min.y,
                plane.normal.z >= 0.0f ? aabb.max.z : aabb.min.z,
            };
            return glm::dot(plane.normal, p) + plane.distance < 0.0f;
        }
    }

    CullResult cull_frustum(const SceneRenderData &scene_data,
                            const std::span<const MaterialInstance> materials) {
        CullResult result;
        const auto &instances = scene_data.mesh_instances;
        const auto planes = extract_frustum_planes(scene_data.camera.view_projection);

        for (uint32_t i = 0; i < static_cast<uint32_t>(instances.size()); ++i) {
            const auto &aabb = instances[i].world_bounds;

            // Test against all 6 frustum planes
            bool outside = false;
            for (const auto &plane: planes) {
                if (aabb_outside_plane(aabb, plane)) {
                    outside = true;
                    break;
                }
            }
            if (outside) continue;

            // Classify by alpha mode
            if (const auto &mat = materials[instances[i].material_id];
                mat.alpha_mode == AlphaMode::Blend) {
                result.visible_transparent_indices.push_back(i);
            } else {
                result.visible_opaque_indices.push_back(i);
            }
        }

        // Sort transparent instances back-to-front by AABB center distance
        if (!result.visible_transparent_indices.empty()) {
            const auto cam_pos = scene_data.camera.position;
            std::ranges::sort(result.visible_transparent_indices,
                              [&](const uint32_t a, const uint32_t b) {
                                  const auto center_a = (instances[a].world_bounds.min +
                                                         instances[a].world_bounds.max) * 0.5f;
                                  const auto center_b = (instances[b].world_bounds.min +
                                                         instances[b].world_bounds.max) * 0.5f;
                                  const float dist_a = glm::dot(center_a - cam_pos, center_a - cam_pos);
                                  const float dist_b = glm::dot(center_b - cam_pos, center_b - cam_pos);
                                  return dist_a > dist_b; // far first
                              });
        }

        return result;
    }
} // namespace himalaya::framework
