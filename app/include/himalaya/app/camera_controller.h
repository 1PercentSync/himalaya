#pragma once

/**
 * @file camera_controller.h
 * @brief Free-roaming camera controller: WASD movement + mouse rotation.
 */

struct GLFWwindow;

namespace himalaya::framework {
    struct Camera;
} // namespace himalaya::framework

namespace himalaya::app {
    /**
     * @brief Free-roaming camera controller with WASD movement and mouse look.
     *
     * Hold right mouse button to rotate the camera. WASD moves along the
     * camera's forward/right directions, Space/Shift moves along world Y.
     * Call update() once per frame after glfwPollEvents().
     */
    class CameraController {
    public:
        /**
         * @brief Binds the controller to a window and camera.
         * @param window GLFW window for input polling.
         * @param camera Camera to control (must outlive the controller).
         */
        void init(GLFWwindow *window, framework::Camera *camera);

        /**
         * @brief Processes input and updates camera matrices.
         * @param delta_time Frame delta time in seconds.
         */
        void update(float delta_time);

        /** @brief Movement speed in world units per second. */
        float move_speed = 5.0f;

        /** @brief Mouse rotation sensitivity in radians per pixel. */
        float mouse_sensitivity = 0.003f;

    private:
        /** @brief GLFW window for input polling. */
        GLFWwindow *window_ = nullptr;

        /** @brief Camera being controlled. */
        framework::Camera *camera_ = nullptr;

        /** @brief Previous cursor X position (for delta calculation). */
        double last_cursor_x_ = 0.0;

        /** @brief Previous cursor Y position (for delta calculation). */
        double last_cursor_y_ = 0.0;

        /** @brief Whether the right mouse button was held last frame. */
        bool right_button_held_ = false;
    };
} // namespace himalaya::app
