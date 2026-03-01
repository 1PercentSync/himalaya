/**
 * @file main.cpp
 * @brief Himalaya renderer application entry point.
 */

#include <GLFW/glfw3.h>

/** @brief Initial window width in pixels. */
constexpr int kInitialWidth = 1280;

/** @brief Initial window height in pixels. */
constexpr int kInitialHeight = 720;

/** @brief Window title shown in the title bar. */
constexpr auto kWindowTitle = "Himalaya";

/**
 * @brief Application entry point.
 *
 * Initializes GLFW, creates a Vulkan-ready window, and runs the main event loop.
 */
int main() {
    glfwInit();
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);

    GLFWwindow *window = glfwCreateWindow(kInitialWidth, kInitialHeight, kWindowTitle, nullptr, nullptr);

    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();
    }

    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
