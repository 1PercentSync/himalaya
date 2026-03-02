#include <himalaya/rhi/context.h>

#include <algorithm>
#include <string>
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

    /** @brief Default log level. Change to debug/info for more verbose Vulkan diagnostics. */
    constexpr auto kLogLevel = spdlog::level::warn;

    /**
     * Derives Vulkan debug messenger severity flags from the spdlog log level,
     * so the validation layer only delivers messages that spdlog would actually display.
     */
    // ReSharper disable once CppDFAConstantParameter
    consteval VkDebugUtilsMessageSeverityFlagsEXT severity_flags_from_log_level(const spdlog::level::level_enum level) {
        VkDebugUtilsMessageSeverityFlagsEXT flags = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        // ReSharper disable once CppDFAConstantConditions
        if (level <= spdlog::level::warn) flags |= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT;
        // ReSharper disable once CppDFAConstantConditions
        // ReSharper disable once CppDFAUnreachableCode
        if (level <= spdlog::level::info) flags |= VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT;
        // ReSharper disable once CppDFAConstantConditions
        // ReSharper disable once CppDFAUnreachableCode
        if (level <= spdlog::level::debug) flags |= VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT;
        return flags;
    }

    void Context::init(GLFWwindow *window) {
        spdlog::set_level(kLogLevel);
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

    // Builds a tag string from the message type bitmask (e.g. "[Validation][Performance]")
    static std::string format_message_type(const VkDebugUtilsMessageTypeFlagsEXT type) {
        std::string tag;
        if (type & VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT) tag += "[Validation]";
        if (type & VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT) tag += "[Performance]";
        return tag;
    }

    // Validation layer message callback, routes to spdlog by severity
    static VKAPI_ATTR VkBool32 VKAPI_CALL debug_callback(
        const VkDebugUtilsMessageSeverityFlagBitsEXT severity,
        const VkDebugUtilsMessageTypeFlagsEXT type,
        const VkDebugUtilsMessengerCallbackDataEXT *callback_data,
        [[maybe_unused]] void *user_data) {
        auto tag = format_message_type(type);
        if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
            spdlog::error("{} {}", tag, callback_data->pMessage);
        } else if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
            spdlog::warn("{} {}", tag, callback_data->pMessage);
        } else if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT) {
            spdlog::info("{} {}", tag, callback_data->pMessage);
        } else {
            spdlog::debug("{} {}", tag, callback_data->pMessage);
        }
        return VK_FALSE;
    }

    void Context::create_debug_messenger() {
        // ReSharper disable once CppDFAUnreachableCode
        if constexpr (!kEnableValidationLayers) return;

        VkDebugUtilsMessengerCreateInfoEXT create_info{};
        create_info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
        create_info.messageSeverity = severity_flags_from_log_level(kLogLevel);
        create_info.messageType =
                // VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |        // loader/layer lifecycle noise
                VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT
                // | VK_DEBUG_UTILS_MESSAGE_TYPE_DEVICE_ADDRESS_BINDING_BIT_EXT        // GPU VA tracking
                ;
        create_info.pfnUserCallback = debug_callback;

        // vkCreateDebugUtilsMessengerEXT is an extension function, must load manually
        const auto func = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT"));
        VK_CHECK(func(instance, &create_info, nullptr, &debug_messenger));

        spdlog::info("Debug messenger created");
    }

    // Checks whether the device has a queue family with VK_QUEUE_GRAPHICS_BIT
    // ReSharper disable once CppParameterMayBeConst
    static bool has_graphics_queue(VkPhysicalDevice dev) {
        uint32_t count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(dev, &count, nullptr);
        std::vector<VkQueueFamilyProperties> families(count);
        vkGetPhysicalDeviceQueueFamilyProperties(dev, &count, families.data());

        return std::ranges::any_of(families, [](const auto &f) {
            return f.queueFlags & VK_QUEUE_GRAPHICS_BIT;
        });
    }

    // Checks whether the device supports VK_KHR_swapchain extension
    // ReSharper disable once CppParameterMayBeConst
    static bool has_swapchain_support(VkPhysicalDevice dev) {
        uint32_t count = 0;
        vkEnumerateDeviceExtensionProperties(dev, nullptr, &count, nullptr);
        std::vector<VkExtensionProperties> extensions(count);
        vkEnumerateDeviceExtensionProperties(dev, nullptr, &count, extensions.data());

        return std::ranges::any_of(extensions, [](const auto &ext) {
            return std::strcmp(ext.extensionName, VK_KHR_SWAPCHAIN_EXTENSION_NAME) == 0;
        });
    }

    /**
     * Rates a physical device's suitability. Returns 0 if unsuitable.
     * Scoring: discrete GPU +1000, then +1 per GB of device-local VRAM.
     * (An iGPU with over 1000 GB VRAM would outscore a discrete GPU — good luck finding one.)
     */
    // ReSharper disable once CppParameterMayBeConst
    static int rate_device(VkPhysicalDevice dev) {
        if (!has_graphics_queue(dev)) return 0;
        if (!has_swapchain_support(dev)) return 0;

        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(dev, &props);

        int score = 1;
        if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
            score += 1000;
        }

        VkPhysicalDeviceMemoryProperties mem_props;
        vkGetPhysicalDeviceMemoryProperties(dev, &mem_props);
        VkDeviceSize max_heap = 0;
        for (uint32_t i = 0; i < mem_props.memoryHeapCount; ++i) {
            if (mem_props.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) {
                max_heap = std::max(max_heap, mem_props.memoryHeaps[i].size);
            }
        }
        score += static_cast<int>(max_heap / (1024 * 1024 * 1024));

        return score;
    }

    void Context::pick_physical_device() {
        uint32_t device_count = 0;
        vkEnumeratePhysicalDevices(instance, &device_count, nullptr);
        if (device_count == 0) {
            spdlog::error("No Vulkan-capable GPU found");
            std::abort();
        }

        std::vector<VkPhysicalDevice> devices(device_count);
        vkEnumeratePhysicalDevices(instance, &device_count, devices.data());

        int best_score = 0;
        for (const auto &dev : devices) {
            if (const int score = rate_device(dev); score > best_score) {
                best_score = score;
                physical_device = dev;
            }
        }

        if (physical_device == VK_NULL_HANDLE) {
            spdlog::error("No suitable GPU found (need graphics queue + swapchain support)");
            std::abort();
        }

        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(physical_device, &props);
        spdlog::info("Selected GPU: {} (score: {})", props.deviceName, best_score);
    }

    void Context::create_device() {
    }

    void Context::create_allocator() {
    }
} // namespace himalaya::rhi
