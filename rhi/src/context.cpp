#include <himalaya/rhi/context.h>

#include <vector>

#include <GLFW/glfw3.h>
#include <spdlog/spdlog.h>

namespace himalaya::rhi {
    /** @brief Application and engine display name. */
    constexpr auto kAppName = "Himalaya";

    /** @brief Current application version. */
    constexpr uint32_t kAppVersion = VK_MAKE_VERSION(0, 0, 1);

#ifdef NDEBUG
    /** @brief Validation layers are disabled in release builds. */
    constexpr bool kEnableValidationLayers = false;
#else
    /** @brief Validation layers are enabled in debug builds. */
    constexpr bool kEnableValidationLayers = true;
#endif

    void Context::init(GLFWwindow *window) {
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
        VkApplicationInfo app_info{};
        app_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        app_info.pApplicationName = kAppName;
        app_info.applicationVersion = kAppVersion;
        app_info.pEngineName = kAppName;
        app_info.engineVersion = kAppVersion;
        app_info.apiVersion = VK_API_VERSION_1_4;

        // Gather GLFW required surface extensions
        uint32_t glfw_extension_count = 0;
        const char **glfw_extensions = glfwGetRequiredInstanceExtensions(&glfw_extension_count);

        std::vector extensions(glfw_extensions, glfw_extensions + glfw_extension_count);
        if (kEnableValidationLayers) {
            extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        }

        const auto validation_layer = "VK_LAYER_KHRONOS_validation";

        VkInstanceCreateInfo create_info{};
        create_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        create_info.pApplicationInfo = &app_info;
        create_info.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
        create_info.ppEnabledExtensionNames = extensions.data();
        if (kEnableValidationLayers) {
            create_info.enabledLayerCount = 1;
            create_info.ppEnabledLayerNames = &validation_layer;
        }

        VK_CHECK(vkCreateInstance(&create_info, nullptr, &instance));

        spdlog::info("Vulkan instance created (API 1.4)");
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
