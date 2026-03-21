#pragma once

/**
 * @file shadow.h
 * @brief CSM shadow cascade computation: PSSM splits, light-space projections, texel snapping.
 *
 * Pure geometric utility — no RHI or Vulkan dependencies.  Renderer consumes
 * the result to fill the GlobalUBO shadow fields each frame.
 */

#include <himalaya/framework/scene_data.h>

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

namespace himalaya::framework {

    struct Camera;

    /**
     * @brief Per-cascade shadow data computed each frame.
     *
     * Returned by compute_shadow_cascades().  Renderer writes fields into
     * the GlobalUniformData UBO and derives PCSS parameters from the
     * orthographic extents.
     */
    struct ShadowCascadeResult {
        /** @brief Per-cascade light-space view-projection matrices (reverse-Z, texel-snapped). */
        glm::mat4 cascade_view_proj[kMaxShadowCascades]{};

        /** @brief Cascade far boundaries in view-space depth. */
        glm::vec4 cascade_splits{};

        /** @brief Per-cascade world-space size of one shadow map texel. */
        glm::vec4 cascade_texel_world_size{};

        /** @brief Per-cascade light-space X extent in world units. */
        glm::vec4 cascade_width_x{};

        /** @brief Per-cascade light-space Y extent in world units. */
        glm::vec4 cascade_width_y{};

        /** @brief Per-cascade light-space Z range after scene AABB extension. */
        glm::vec4 cascade_depth_range{};
    };

    /**
     * @brief Computes CSM cascade data for one frame.
     *
     * Uses PSSM (Practical Split Scheme) to distribute cascades:
     * C_i = lambda * C_log + (1 - lambda) * C_linear.  Each cascade's
     * orthographic projection tightly fits its camera sub-frustum corners
     * in XY, with Z extended to the scene AABB to capture shadow casters
     * outside the frustum.  Texel snapping prevents shadow edge shimmer
     * during camera translation.
     *
     * @param cam              Camera state (position, orientation, FOV, planes).
     * @param light_dir        Normalized light direction (toward scene).
     * @param config           Shadow configuration (cascade_count, split_lambda, max_distance).
     * @param scene_bounds     World-space scene AABB for Z range extension.
     * @param shadow_texel_size  1.0 / shadow_map_resolution.
     * @return Per-cascade VP matrices, split distances, texel sizes, and orthographic extents.
     */
    ShadowCascadeResult compute_shadow_cascades(
        const Camera &cam,
        const glm::vec3 &light_dir,
        const ShadowConfig &config,
        const AABB &scene_bounds,
        float shadow_texel_size);

} // namespace himalaya::framework
