#include <GLFW/glfw3.h>
#include <cstdlib>

constexpr int kInitialWidth = 1280;
constexpr int kInitialHeight = 720;
constexpr const char* kWindowTitle = "Himalaya";

int main() {
    glfwInit();
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);

    GLFWwindow* window = glfwCreateWindow(kInitialWidth, kInitialHeight, kWindowTitle, nullptr, nullptr);

    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();
    }

    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
