/**
 * @file shadow.cpp
 * @brief CSM shadow cascade computation implementation.
 *
 * PSSM split distribution, per-cascade tight orthographic fitting,
 * scene AABB Z extension, texel snapping, and texel world size calculation.
 */

#include <himalaya/framework/shadow.h>

#include <himalaya/framework/camera.h>

#include <array>
#include <cmath>
#include <limits>

#include <glm/gtc/matrix_transform.hpp>

namespace himalaya::framework {
    namespace {
        /**
         * Builds a reverse-Z orthographic projection matrix.
         *
         * Standard glm::orthoRH_ZO maps [near, far] -> [0, 1]. This function
         * flips the mapping to [1, 0] (near -> 1, far -> 0) to match the
         * project-wide reverse-Z convention (clear 0.0, compare GREATER).
         *
         * Derivation: applying depth_new = 1 - depth_old to the standard
         * mapping yields M[2][2] = -M[2][2] and M[3][2] = 1 - M[3][2].
         */
        glm::mat4 ortho_reverse_z(const float left,
                                   const float right,
                                   const float bottom,
                                   const float top,
                                   const float z_near,
                                   const float z_far) {
            glm::mat4 m = glm::orthoRH_ZO(left, right, bottom, top, z_near, z_far);
            m[2][2] = -m[2][2];
            m[3][2] = 1.0f - m[3][2];
            return m;
        }
    } // anonymous namespace

    ShadowCascadeResult compute_shadow_cascades(
        const Camera &cam,
        const glm::vec3 &light_dir,
        const ShadowConfig &config,
        const AABB &scene_bounds,
        const float shadow_texel_size) {
        ShadowCascadeResult result;
        const float shadow_far = std::min(config.max_distance, cam.far_plane);
        const uint32_t n = config.cascade_count;

        // Camera basis vectors
        const glm::vec3 fwd = cam.forward();
        const glm::vec3 rgt = cam.right();
        const glm::vec3 up = glm::cross(rgt, fwd);
        const float tan_half = std::tan(cam.fov * 0.5f);

        // --- PSSM split distances ---
        // splits[0] = near_plane, splits[1..n] = cascade far boundaries.
        // C_log_i = near * (far/near)^(i/n)
        // C_lin_i = near + (far - near) * i/n
        // C_i = lambda * C_log + (1 - lambda) * C_lin
        std::array<float, kMaxShadowCascades + 1> splits{}; // cascades + near
        splits[0] = cam.near_plane;
        for (uint32_t i = 1; i <= n; ++i) {
            const float t = static_cast<float>(i) / static_cast<float>(n);
            const float c_log = cam.near_plane * std::pow(shadow_far / cam.near_plane, t);
            const float c_lin = cam.near_plane + (shadow_far - cam.near_plane) * t;
            splits[i] = config.split_lambda * c_log + (1.0f - config.split_lambda) * c_lin;
        }

        for (uint32_t i = 0; i < n; ++i) {
            result.cascade_splits[static_cast<int>(i)] = splits[i + 1];
        }

        // --- Light-space basis (shared across all cascades) ---
        const glm::vec3 ref = std::abs(light_dir.y) < 0.999f
                                  ? glm::vec3(0.0f, 1.0f, 0.0f)
                                  : glm::vec3(1.0f, 0.0f, 0.0f);
        const glm::vec3 light_right = glm::normalize(glm::cross(light_dir, ref));
        const glm::vec3 light_up = glm::cross(light_right, light_dir);

        // Scene AABB corners for Z extension (shadow casters outside frustum)
        const std::array<glm::vec3, 8> scene_corners = {
            glm::vec3{scene_bounds.min.x, scene_bounds.min.y, scene_bounds.min.z},
            glm::vec3{scene_bounds.max.x, scene_bounds.min.y, scene_bounds.min.z},
            glm::vec3{scene_bounds.min.x, scene_bounds.max.y, scene_bounds.min.z},
            glm::vec3{scene_bounds.max.x, scene_bounds.max.y, scene_bounds.min.z},
            glm::vec3{scene_bounds.min.x, scene_bounds.min.y, scene_bounds.max.z},
            glm::vec3{scene_bounds.max.x, scene_bounds.min.y, scene_bounds.max.z},
            glm::vec3{scene_bounds.min.x, scene_bounds.max.y, scene_bounds.max.z},
            glm::vec3{scene_bounds.max.x, scene_bounds.max.y, scene_bounds.max.z},
        };

        // --- Per-cascade projection computation ---
        for (uint32_t c = 0; c < n; ++c) {
            const float c_near = splits[c];
            const float c_far = splits[c + 1];

            // Camera sub-frustum corners for this cascade slice
            const float nh = c_near * tan_half;
            const float nw = nh * cam.aspect;
            const float fh = c_far * tan_half;
            const float fw = fh * cam.aspect;

            const glm::vec3 nc = cam.position + fwd * c_near;
            const glm::vec3 fc = cam.position + fwd * c_far;

            const std::array<glm::vec3, 8> corners = {
                nc - rgt * nw + up * nh,
                nc + rgt * nw + up * nh,
                nc - rgt * nw - up * nh,
                nc + rgt * nw - up * nh,
                fc - rgt * fw + up * fh,
                fc + rgt * fw + up * fh,
                fc - rgt * fw - up * fh,
                fc + rgt * fw - up * fh,
            };

            // Sub-frustum center — light-view origin for numerical precision
            glm::vec3 center(0.0f);
            for (const auto &corner: corners) {
                center += corner;
            }
            center *= (1.0f / 8.0f);

            // Light-view matrix centered on this cascade's sub-frustum
            const glm::mat4 light_view(
                glm::vec4(light_right.x, light_up.x, -light_dir.x, 0.0f),
                glm::vec4(light_right.y, light_up.y, -light_dir.y, 0.0f),
                glm::vec4(light_right.z, light_up.z, -light_dir.z, 0.0f),
                glm::vec4(-glm::dot(light_right, center),
                          -glm::dot(light_up, center),
                          glm::dot(light_dir, center), 1.0f));

            // Tight AABB of sub-frustum corners in light space (XY fit)
            glm::vec3 ls_min(std::numeric_limits<float>::max());
            glm::vec3 ls_max(std::numeric_limits<float>::lowest());
            for (const auto &corner: corners) {
                const auto ls = glm::vec3(light_view * glm::vec4(corner, 1.0f));
                ls_min = glm::min(ls_min, ls);
                ls_max = glm::max(ls_max, ls);
            }

            // Extend Z to scene AABB (shadow casters outside this frustum slice)
            for (const auto &sc: scene_corners) {
                const float lz = glm::vec3(light_view * glm::vec4(sc, 1.0f)).z;
                ls_min.z = std::min(ls_min.z, lz);
                ls_max.z = std::max(ls_max.z, lz);
            }

            // Store per-cascade orthographic extents for PCSS parameter computation
            const auto ci = static_cast<int>(c);
            result.cascade_width_x[ci] = ls_max.x - ls_min.x;
            result.cascade_width_y[ci] = ls_max.y - ls_min.y;
            result.cascade_depth_range[ci] = ls_max.z - ls_min.z;

            // Orthographic projection: XY tight fit, Z from scene AABB
            const glm::mat4 light_proj = ortho_reverse_z(
                ls_min.x, ls_max.x,
                ls_min.y, ls_max.y,
                -ls_max.z, -ls_min.z);

            result.cascade_view_proj[c] = light_proj * light_view;

            // Texel snapping: align the VP offset to shadow map texel
            // boundaries, preventing shadow edge shimmer when the camera
            // translates.  Standard technique: project world origin through
            // the combined VP, round to the nearest texel center, and apply
            // the resulting sub-texel correction to the VP translation.
            {
                const float resolution = 1.0f / shadow_texel_size;
                const float half_res = resolution * 0.5f;
                auto &vp = result.cascade_view_proj[c];

                // VP * (0,0,0,1) = translation column; ortho => w=1
                const float sx = vp[3][0] * half_res;
                const float sy = vp[3][1] * half_res;
                vp[3][0] += (std::round(sx) - sx) / half_res;
                vp[3][1] += (std::round(sy) - sy) / half_res;
            }

            // Per-cascade texel world size: clip [-1,1] covers
            // (2 / ||row0||) world units; divided by resolution = per-texel size.
            const glm::vec3 row0(result.cascade_view_proj[c][0][0],
                                 result.cascade_view_proj[c][1][0],
                                 result.cascade_view_proj[c][2][0]);
            result.cascade_texel_world_size[static_cast<int>(c)] =
                    2.0f * shadow_texel_size / glm::length(row0);
        }

        return result;
    }
} // namespace himalaya::framework
