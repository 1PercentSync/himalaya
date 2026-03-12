#include <himalaya/rhi/context.h>

#include <algorithm>
#include <cassert>
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

    /** @brief Device extensions required by the renderer. */
    constexpr const char *kRequiredDeviceExtensions[] = {
        VK_KHR_SWAPCHAIN_EXTENSION_NAME,
        VK_EXT_MEMORY_BUDGET_EXTENSION_NAME,
    };

    /** @brief All severity levels — let the callback do spdlog-level filtering at runtime. */
    constexpr VkDebugUtilsMessageSeverityFlagsEXT kAllSeverityFlags =
        VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT |
        VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT |
        VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
        VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;

    void Context::init(GLFWwindow *window) {
        create_instance();
        create_debug_messenger();
        VK_CHECK(glfwCreateWindowSurface(instance, window, nullptr, &surface));
        pick_physical_device();
        create_device();
        create_allocator();
        create_frame_data();
        create_immediate_pool();
    }

    void Context::destroy() {
        vkDestroyCommandPool(device, immediate_command_pool, nullptr);

        for (auto &frame: frames) {
            frame.deletion_queue.flush();
            vkDestroyCommandPool(device, frame.command_pool, nullptr);
            vkDestroyFence(device, frame.render_fence, nullptr);
            vkDestroySemaphore(device, frame.image_available_semaphore, nullptr);
        }

        vmaDestroyAllocator(allocator);
        vkDestroyDevice(device, nullptr);
        vkDestroySurfaceKHR(instance, surface, nullptr);

        if constexpr (kEnableValidationLayers) {
            const auto func = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
                vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT")
            );
            func(instance, debug_messenger, nullptr);
        }

        vkDestroyInstance(instance, nullptr);

        spdlog::info("Vulkan context destroyed");
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
        create_info.messageSeverity = kAllSeverityFlags;
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

    // Checks whether the device has a queue family supporting both graphics and present
    // ReSharper disable CppParameterMayBeConst
    static bool has_graphics_present_queue(VkPhysicalDevice dev, VkSurfaceKHR surface) {
        // ReSharper restore CppParameterMayBeConst
        uint32_t count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(dev, &count, nullptr);
        std::vector<VkQueueFamilyProperties> families(count);
        vkGetPhysicalDeviceQueueFamilyProperties(dev, &count, families.data());

        for (uint32_t i = 0; i < count; ++i) {
            if (!(families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) continue;

            VkBool32 present_support = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(dev, i, surface, &present_support);
            if (present_support) return true;
        }
        return false;
    }

    // Checks whether the device supports all extensions in kRequiredDeviceExtensions
    // ReSharper disable once CppParameterMayBeConst
    static bool has_required_extensions(VkPhysicalDevice dev) {
        uint32_t count = 0;
        vkEnumerateDeviceExtensionProperties(dev, nullptr, &count, nullptr);
        std::vector<VkExtensionProperties> available(count);
        vkEnumerateDeviceExtensionProperties(dev, nullptr, &count, available.data());

        for (const auto *required: kRequiredDeviceExtensions) {
            const bool found = std::ranges::any_of(available, [required](const auto &ext) {
                return std::strcmp(ext.extensionName, required) == 0;
            });
            if (!found) return false;
        }
        return true;
    }

    /**
     * Rates a physical device's suitability. Returns 0 if unsuitable.
     * Scoring: discrete GPU +1000, then +1 per GB of device-local VRAM.
     * (An iGPU with over 1000 GB VRAM would outscore a discrete GPU — good luck finding one.)
     */
    // ReSharper disable once CppParameterMayBeConst
    static int rate_device(VkPhysicalDevice dev, VkSurfaceKHR surface) {
        if (!has_graphics_present_queue(dev, surface)) return 0;
        if (!has_required_extensions(dev)) return 0;

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
        for (const auto &dev: devices) {
            if (const int score = rate_device(dev, surface); score > best_score) {
                best_score = score;
                physical_device = dev;
            }
        }

        if (physical_device == VK_NULL_HANDLE) {
            spdlog::error("No suitable GPU found (need graphics+present queue + required extensions)");
            std::abort();
        }

        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(physical_device, &props);
        gpu_name = props.deviceName;
        max_sampler_anisotropy = props.limits.maxSamplerAnisotropy;

        // Compute max MSAA sample count supported by both color and depth framebuffers
        const VkSampleCountFlags counts = props.limits.framebufferColorSampleCounts &
                                           props.limits.framebufferDepthSampleCounts;
        if (counts & VK_SAMPLE_COUNT_64_BIT) max_msaa_samples = 64;
        else if (counts & VK_SAMPLE_COUNT_32_BIT) max_msaa_samples = 32;
        else if (counts & VK_SAMPLE_COUNT_16_BIT) max_msaa_samples = 16;
        else if (counts & VK_SAMPLE_COUNT_8_BIT) max_msaa_samples = 8;
        else if (counts & VK_SAMPLE_COUNT_4_BIT) max_msaa_samples = 4;
        else if (counts & VK_SAMPLE_COUNT_2_BIT) max_msaa_samples = 2;
        else max_msaa_samples = 1;

        spdlog::info("Selected GPU: {} (score: {}, max MSAA: {}x)", gpu_name, best_score, max_msaa_samples);
    }

    void Context::create_device() {
        // Find a queue family that supports both graphics and present
        uint32_t family_count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(physical_device, &family_count, nullptr);
        std::vector<VkQueueFamilyProperties> families(family_count);
        vkGetPhysicalDeviceQueueFamilyProperties(physical_device, &family_count, families.data());

        bool found = false;
        for (uint32_t i = 0; i < family_count; ++i) {
            if (!(families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) continue;

            VkBool32 present_support = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(physical_device, i, surface, &present_support);
            if (!present_support) continue;

            graphics_queue_family = i;
            found = true;
            break;
        }

        if (!found) {
            spdlog::error("No queue family supports both graphics and present "
                "(unexpected: should have been filtered by pick_physical_device)");
            std::abort();
        }

        constexpr float queue_priority = 1.0f;
        VkDeviceQueueCreateInfo queue_info{};
        queue_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queue_info.queueFamilyIndex = graphics_queue_family;
        queue_info.queueCount = 1;
        queue_info.pQueuePriorities = &queue_priority;

        // Vulkan 1.2 core features: descriptor indexing for bindless textures
        VkPhysicalDeviceVulkan12Features features_12{};
        features_12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
        features_12.descriptorBindingPartiallyBound = VK_TRUE;
        features_12.descriptorBindingSampledImageUpdateAfterBind = VK_TRUE;
        features_12.runtimeDescriptorArray = VK_TRUE;
        features_12.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;

        // Vulkan 1.3 core features: dynamic rendering + synchronization2 + demote-to-helper
        // Extended Dynamic State is unconditionally available in 1.3+ (no feature bit needed)
        VkPhysicalDeviceVulkan13Features features_13{};
        features_13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
        features_13.dynamicRendering = VK_TRUE;
        features_13.synchronization2 = VK_TRUE;
        features_13.shaderDemoteToHelperInvocation = VK_TRUE;

        // Vulkan 1.4 core features: push descriptors (IBL compute dispatch)
        VkPhysicalDeviceVulkan14Features features_14{};
        features_14.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES;
        features_14.pushDescriptor = VK_TRUE;

        // Vulkan 1.0 core features
        VkPhysicalDeviceFeatures features_10{};
        features_10.samplerAnisotropy = VK_TRUE;

        features_13.pNext = &features_14;
        features_12.pNext = &features_13;

        VkDeviceCreateInfo device_info{};
        device_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        device_info.pNext = &features_12;
        device_info.pEnabledFeatures = &features_10;
        device_info.queueCreateInfoCount = 1;
        device_info.pQueueCreateInfos = &queue_info;
        device_info.enabledExtensionCount = static_cast<uint32_t>(std::size(kRequiredDeviceExtensions));
        device_info.ppEnabledExtensionNames = kRequiredDeviceExtensions;

        VK_CHECK(vkCreateDevice(physical_device, &device_info, nullptr, &device));

        vkGetDeviceQueue(device, graphics_queue_family, 0, &graphics_queue);

        spdlog::info("Logical device created (queue family: {})", graphics_queue_family);
    }

    void Context::create_allocator() {
        VmaAllocatorCreateInfo alloc_info{};
        alloc_info.flags = VMA_ALLOCATOR_CREATE_EXT_MEMORY_BUDGET_BIT;
        alloc_info.physicalDevice = physical_device;
        alloc_info.device = device;
        alloc_info.instance = instance;
        alloc_info.vulkanApiVersion = VK_API_VERSION_1_4;

        VK_CHECK(vmaCreateAllocator(&alloc_info, &allocator));

        spdlog::info("VMA allocator created");
    }

    // Creates per-frame command pools, command buffers, fences (signaled), and semaphores.
    // Fences start signaled so the first frame's wait_fence succeeds immediately.
    void Context::create_frame_data() {
        // ReSharper disable once CppUseStructuredBinding
        for (auto &frame: frames) {
            VkCommandPoolCreateInfo pool_info{};
            pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
            pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
            pool_info.queueFamilyIndex = graphics_queue_family;

            VK_CHECK(vkCreateCommandPool(device, &pool_info, nullptr, &frame.command_pool));

            VkCommandBufferAllocateInfo alloc_info{};
            alloc_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
            alloc_info.commandPool = frame.command_pool;
            alloc_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            alloc_info.commandBufferCount = 1;

            VK_CHECK(vkAllocateCommandBuffers(device, &alloc_info, &frame.command_buffer));

            VkFenceCreateInfo fence_info{};
            fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
            fence_info.flags = VK_FENCE_CREATE_SIGNALED_BIT;

            VK_CHECK(vkCreateFence(device, &fence_info, nullptr, &frame.render_fence));

            VkSemaphoreCreateInfo semaphore_info{};
            semaphore_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

            VK_CHECK(vkCreateSemaphore(device, &semaphore_info, nullptr, &frame.image_available_semaphore));
        }

        spdlog::info("Frame data created ({} frames in flight)", kMaxFramesInFlight);
    }

    // Creates a dedicated command pool for one-shot blocking GPU operations (uploads, etc.).
    // Uses TRANSIENT_BIT: command buffers are short-lived (begin → record → submit → reset).
    void Context::create_immediate_pool() {
        VkCommandPoolCreateInfo pool_info{};
        pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pool_info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT
                          | VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pool_info.queueFamilyIndex = graphics_queue_family;

        VK_CHECK(vkCreateCommandPool(device, &pool_info, nullptr, &immediate_command_pool));

        VkCommandBufferAllocateInfo alloc_info{};
        alloc_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        alloc_info.commandPool = immediate_command_pool;
        alloc_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        alloc_info.commandBufferCount = 1;

        VK_CHECK(vkAllocateCommandBuffers(device, &alloc_info, &immediate_command_buffer));

        spdlog::info("Immediate command pool created");
    }

    // Resets and begins the immediate command buffer for batch recording.
    // Upload methods record into this buffer; end_immediate() submits everything at once.
    void Context::begin_immediate() {
        assert(!immediate_active_ && "Immediate scope already active");
        immediate_active_ = true;

        VK_CHECK(vkResetCommandBuffer(immediate_command_buffer, 0));

        VkCommandBufferBeginInfo begin_info{};
        begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        VK_CHECK(vkBeginCommandBuffer(immediate_command_buffer, &begin_info));
    }

    // Submits the recorded immediate commands, waits for GPU completion,
    // then destroys all staging buffers collected during the scope.
    void Context::end_immediate() {
        assert(immediate_active_ && "No active immediate scope");

        VK_CHECK(vkEndCommandBuffer(immediate_command_buffer));

        VkSubmitInfo submit_info{};
        submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit_info.commandBufferCount = 1;
        submit_info.pCommandBuffers = &immediate_command_buffer;

        VK_CHECK(vkQueueSubmit(graphics_queue, 1, &submit_info, VK_NULL_HANDLE));
        VK_CHECK(vkQueueWaitIdle(graphics_queue));

        for (auto &[buffer, allocation]: staging_buffers_) {
            vmaDestroyBuffer(allocator, buffer, allocation);
        }
        staging_buffers_.clear();
        immediate_active_ = false;
    }

    // ReSharper disable CppParameterMayBeConst
    void Context::push_staging_buffer(VkBuffer buffer, VmaAllocation allocation) {
        // ReSharper restore CppParameterMayBeConst
        assert(immediate_active_ && "push_staging_buffer requires an active immediate scope");
        staging_buffers_.push_back({buffer, allocation});
    }

    // Sums usage and budget across all device-local heaps.
    // VK_EXT_memory_budget provides the per-heap budget/usage via VMA.
    VramInfo Context::query_vram_usage() const {
        VmaBudget budgets[VK_MAX_MEMORY_HEAPS];
        vmaGetHeapBudgets(allocator, budgets);

        VkPhysicalDeviceMemoryProperties mem_props;
        vkGetPhysicalDeviceMemoryProperties(physical_device, &mem_props);

        VramInfo info;
        for (uint32_t i = 0; i < mem_props.memoryHeapCount; ++i) {
            if (mem_props.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) {
                info.used += budgets[i].usage;
                info.budget += budgets[i].budget;
            }
        }
        return info;
    }
} // namespace himalaya::rhi
