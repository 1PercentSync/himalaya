#include <himalaya/framework/camera.h>
#include <himalaya/framework/scene_data.h>

#include <glm/gtc/matrix_transform.hpp>

#include <cmath>

namespace himalaya::framework {

    void Camera::update_view() {
        const glm::vec3 fwd = forward();
        view = glm::lookAt(position, position + fwd, glm::vec3(0.0f, 1.0f, 0.0f));
    }

    void Camera::update_projection() {
        // Reverse-Z perspective: near maps to depth 1, far maps to depth 0.
        // Derived from z_ndc = (A * z_eye + B) / (-z_eye) with:
        //   z_ndc(near) = 1, z_ndc(far) = 0
        //   => A = near / (far - near), B = near * far / (far - near)
        const float f = 1.0f / std::tan(fov * 0.5f);

        projection = glm::mat4(0.0f);
        projection[0][0] = f / aspect;
        projection[1][1] = f;
        projection[2][2] = near_plane / (far_plane - near_plane);
        projection[2][3] = -1.0f;
        projection[3][2] = near_plane * far_plane / (far_plane - near_plane);
    }

    void Camera::update_view_projection() {
        view_projection = projection * view;
        inv_view_projection = glm::inverse(view_projection);
    }

    void Camera::update_all() {
        update_view();
        update_projection();
        update_view_projection();
    }

    glm::vec3 Camera::compute_focus_position(const AABB &bounds) const {
        const glm::vec3 center = (bounds.min + bounds.max) * 0.5f;
        const float diagonal = glm::length(bounds.max - bounds.min);

        constexpr float kEpsilon = 1e-4f;
        if (diagonal < kEpsilon) {
            return position;
        }

        const float radius = diagonal * 0.5f;
        // Use the tighter of vertical and horizontal FOV so the bounding
        // sphere fits in both dimensions (matters for narrow viewports).
        const float half_v = fov * 0.5f;
        const float half_h = std::atan(std::tan(half_v) * aspect);
        const float distance = radius / std::sin(std::min(half_v, half_h));
        return center - forward() * distance;
    }

    glm::vec3 Camera::forward() const {
        // yaw=0, pitch=0 => (0, 0, -1), looking along -Z
        return {
            std::sin(yaw) * std::cos(pitch),
            std::sin(pitch),
            -std::cos(yaw) * std::cos(pitch)
        };
    }

    glm::vec3 Camera::right() const {
        // Always horizontal (no roll). yaw=0 => (1, 0, 0)
        return {
            std::cos(yaw),
            0.0f,
            std::sin(yaw)
        };
    }

} // namespace himalaya::framework
