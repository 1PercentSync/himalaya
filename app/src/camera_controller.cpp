#include <himalaya/app/camera_controller.h>
#include <himalaya/framework/camera.h>
#include <himalaya/framework/scene_data.h>

#include <glm/glm.hpp>

#include <GLFW/glfw3.h>
#include <imgui.h>

#include <algorithm>

namespace himalaya::app {
    /** @brief Sprint speed multiplier when Shift is held. */
    constexpr float kSprintMultiplier = 3.0f;

    void CameraController::init(GLFWwindow *window, framework::Camera *camera) {
        window_ = window;
        camera_ = camera;
    }

    void CameraController::set_focus_target(const framework::AABB *bounds) {
        focus_target_ = bounds;
    }

    void CameraController::update(const float delta_time) {
        const ImGuiIO &io = ImGui::GetIO();

        // --- Mouse rotation (right-click hold) ---

        // Once rotation starts, ignore WantCaptureMouse — the virtual cursor
        // in CURSOR_DISABLED mode can drift over ImGui widgets, causing
        // flickering capture state and sudden angle jumps.
        const bool right_down = glfwGetMouseButton(window_, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS;
        const bool right_pressed = right_button_held_
                                       ? right_down
                                       : (right_down && !io.WantCaptureMouse);

        double cursor_x, cursor_y;
        glfwGetCursorPos(window_, &cursor_x, &cursor_y);

        if (right_pressed) {
            if (!right_button_held_) {
                // Just pressed: hide cursor, enable raw mouse motion if available
                right_button_held_ = true;
                glfwSetInputMode(window_, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
                if (glfwRawMouseMotionSupported()) {
                    glfwSetInputMode(window_, GLFW_RAW_MOUSE_MOTION, GLFW_TRUE);
                }
                // Re-read position after mode change to avoid coordinate mismatch
                glfwGetCursorPos(window_, &cursor_x, &cursor_y);
                last_cursor_x_ = cursor_x;
                last_cursor_y_ = cursor_y;
            }

            const auto dx = static_cast<float>(cursor_x - last_cursor_x_);
            const auto dy = static_cast<float>(cursor_y - last_cursor_y_);
            last_cursor_x_ = cursor_x;
            last_cursor_y_ = cursor_y;

            camera_->yaw += dx * mouse_sensitivity;
            camera_->pitch -= dy * mouse_sensitivity;

            constexpr float kMaxPitch = glm::radians(89.0f);
            camera_->pitch = std::clamp(camera_->pitch, -kMaxPitch, kMaxPitch);
        } else if (right_button_held_) {
            // Just released: restore cursor
            right_button_held_ = false;
            glfwSetInputMode(window_, GLFW_RAW_MOUSE_MOTION, GLFW_FALSE);
            glfwSetInputMode(window_, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
        }

        // --- Keyboard movement ---

        // WantTextInput (not WantCaptureKeyboard) — with NavEnableKeyboard,
        // WantCaptureKeyboard is true whenever any widget is focused, which
        // blocks WASD permanently. WantTextInput only fires during actual
        // text entry (e.g. Ctrl+Click on a slider).
        if (!io.WantTextInput) {
            const glm::vec3 forward = camera_->forward();
            const glm::vec3 right = camera_->right();
            constexpr glm::vec3 world_up{0.0f, 1.0f, 0.0f};

            glm::vec3 move{0.0f};

            if (glfwGetKey(window_, GLFW_KEY_W) == GLFW_PRESS) move += forward;
            if (glfwGetKey(window_, GLFW_KEY_S) == GLFW_PRESS) move -= forward;
            if (glfwGetKey(window_, GLFW_KEY_D) == GLFW_PRESS) move += right;
            if (glfwGetKey(window_, GLFW_KEY_A) == GLFW_PRESS) move -= right;
            if (glfwGetKey(window_, GLFW_KEY_SPACE) == GLFW_PRESS) move += world_up;
            if (glfwGetKey(window_, GLFW_KEY_LEFT_CONTROL) == GLFW_PRESS) move -= world_up;

            if (glm::dot(move, move) > 0.0f) {
                const float speed = glfwGetKey(window_, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS
                                        ? move_speed * kSprintMultiplier
                                        : move_speed;
                camera_->position += glm::normalize(move) * speed * delta_time;
            }

            // F-key focus: move to frame the scene AABB, keep current orientation
            if (focus_target_ && ImGui::IsKeyPressed(ImGuiKey_F, false)) {
                camera_->position = camera_->compute_focus_position(*focus_target_);
            }
        }

        // --- Update camera matrices ---

        camera_->update_all();
    }
} // namespace himalaya::app
