#pragma once

/**
 * @file camera.h
 * @brief Camera data and projection/view matrix computation.
 */

#include <glm/glm.hpp>

namespace himalaya::framework {

    struct AABB;

    /**
     * @brief Camera state: orientation, projection parameters, and derived matrices.
     *
     * Stores input state (position, yaw/pitch, fov, near/far, aspect) and derived
     * state (view, projection, view_projection, inv_view_projection matrices).
     * Call update methods after modifying input state to recompute matrices.
     *
     * Uses reverse-Z projection: near plane maps to depth 1, far plane maps to
     * depth 0. Requires GLM_FORCE_DEPTH_ZERO_TO_ONE and VK_COMPARE_OP_GREATER.
     *
     * Coordinate system: right-handed, Y-up. Yaw = 0 looks along -Z.
     */
    struct Camera {
        // --- Input state (modify directly, then call update methods) ---

        /** @brief World-space position. */
        glm::vec3 position{0.0f, 0.0f, 3.0f};

        /** @brief Horizontal rotation in radians (0 = looking along -Z). */
        float yaw = 0.0f;

        /** @brief Vertical rotation in radians (0 = horizontal, positive = up). */
        float pitch = 0.0f;

        /** @brief Vertical field of view in radians. */
        float fov = glm::radians(60.0f);

        /** @brief Distance to the near clipping plane. */
        float near_plane = 0.1f;

        /** @brief Distance to the far clipping plane. */
        float far_plane = 1000.0f;

        /** @brief Viewport aspect ratio (width / height). */
        float aspect = 16.0f / 9.0f;

        // --- Derived state (computed by update methods) ---

        /** @brief View matrix (world to view space). */
        glm::mat4 view{1.0f};

        /** @brief Projection matrix (view to clip space, reverse-Z). */
        glm::mat4 projection{1.0f};

        /** @brief Combined view-projection matrix. */
        glm::mat4 view_projection{1.0f};

        /** @brief Inverse of view_projection (clip to world space). */
        glm::mat4 inv_view_projection{1.0f};

        // --- Update methods ---

        /**
         * @brief Recomputes the view matrix from position, yaw, and pitch.
         */
        void update_view();

        /**
         * @brief Recomputes the reverse-Z perspective projection matrix.
         *
         * Near plane maps to depth 1, far plane maps to depth 0.
         */
        void update_projection();

        /**
         * @brief Recomputes view_projection and inv_view_projection.
         *
         * Must be called after update_view() and/or update_projection().
         */
        void update_view_projection();

        /**
         * @brief Convenience: update_view() + update_projection() + update_view_projection().
         */
        void update_all();

        // --- Focus helpers ---

        /**
         * @brief Computes a camera position that frames the given AABB.
         *
         * Uses current yaw, pitch, and fov to calculate the distance needed
         * to fit the AABB's bounding sphere in view. Does not modify camera state.
         * Returns current position unchanged if the AABB is degenerate (diagonal ~ 0).
         */
        [[nodiscard]] glm::vec3 compute_focus_position(const AABB &bounds) const;

        // --- Direction helpers ---

        /** @brief Forward direction vector derived from yaw and pitch. */
        [[nodiscard]] glm::vec3 forward() const;

        /** @brief Right direction vector (always horizontal, perpendicular to forward). */
        [[nodiscard]] glm::vec3 right() const;
    };

} // namespace himalaya::framework
