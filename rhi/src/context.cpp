#include <himalaya/rhi/context.h>

#include <GLFW/glfw3.h>
#include <spdlog/spdlog.h>

namespace himalaya::rhi {

void Context::init(GLFWwindow* window) {
    create_instance();
    create_debug_messenger();
    VK_CHECK(glfwCreateWindowSurface(instance, window, nullptr, &surface));
    pick_physical_device();
    create_device();
    create_allocator();
}

void Context::destroy() {
}

void Context::create_instance() {
}

void Context::create_debug_messenger() {
}

void Context::pick_physical_device() {
}

void Context::create_device() {
}

void Context::create_allocator() {
}

} // namespace himalaya::rhi
