#include <himalaya/rhi/swapchain.h>
#include <himalaya/rhi/context.h>

#include <algorithm>
#include <limits>

#include <GLFW/glfw3.h>
#include <spdlog/spdlog.h>

namespace himalaya::rhi {
    void Swapchain::init(const Context &context, GLFWwindow *window) {
        VkSurfaceCapabilitiesKHR capabilities;
        vkGetPhysicalDeviceSurfaceCapabilitiesKHR(context.physical_device, context.surface, &capabilities);

        const auto [surface_format, color_space] = choose_surface_format(context.physical_device, context.surface);
        const auto present_mode = choose_present_mode(context.physical_device, context.surface);
        extent = choose_extent(capabilities, window);
        format = surface_format;

        // Request one more image than the minimum for triple-buffering headroom;
        // clamp to maxImageCount (0 means unlimited)
        uint32_t image_count = capabilities.minImageCount + 1;
        if (capabilities.maxImageCount > 0 && image_count > capabilities.maxImageCount) {
            image_count = capabilities.maxImageCount;
        }

        VkSwapchainCreateInfoKHR create_info{};
        create_info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
        create_info.surface = context.surface;
        create_info.minImageCount = image_count;
        create_info.imageFormat = format;
        create_info.imageColorSpace = color_space;
        create_info.imageExtent = extent;
        create_info.imageArrayLayers = 1;
        create_info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        create_info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        create_info.preTransform = capabilities.currentTransform;
        create_info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        create_info.presentMode = present_mode;
        create_info.clipped = VK_TRUE;

        VK_CHECK(vkCreateSwapchainKHR(context.device, &create_info, nullptr, &swapchain));

        // Retrieve swapchain images (owned by the swapchain, not manually destroyed)
        vkGetSwapchainImagesKHR(context.device, swapchain, &image_count, nullptr);
        images.resize(image_count);
        vkGetSwapchainImagesKHR(context.device, swapchain, &image_count, images.data());

        spdlog::info("Swapchain created ({}x{}, {} images)",
                     extent.width,
                     extent.height,
                     image_count);
    }

    // ReSharper disable once CppParameterMayBeConst
    void Swapchain::destroy(VkDevice device) const {
        for (const auto view: image_views) {
            vkDestroyImageView(device, view, nullptr);
        }
        vkDestroySwapchainKHR(device, swapchain, nullptr);

        spdlog::info("Swapchain destroyed");
    }

    // Queries available surface formats and picks the preferred one
    // ReSharper disable CppParameterMayBeConst
    VkSurfaceFormatKHR Swapchain::choose_surface_format(VkPhysicalDevice physical_device, VkSurfaceKHR surface) {
        // ReSharper restore CppParameterMayBeConst
        uint32_t count = 0;
        vkGetPhysicalDeviceSurfaceFormatsKHR(physical_device, surface, &count, nullptr);
        std::vector<VkSurfaceFormatKHR> formats(count);
        vkGetPhysicalDeviceSurfaceFormatsKHR(physical_device, surface, &count, formats.data());

        for (const auto &fmt: formats) {
            if (fmt.format == VK_FORMAT_B8G8R8A8_SRGB && fmt.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
                return fmt;
            }
        }

        spdlog::warn("Preferred surface format B8G8R8A8_SRGB not available, using first format");
        return formats[0];
    }

    // Queries available present modes; MAILBOX for low-latency, FIFO as guaranteed fallback
    // ReSharper disable CppParameterMayBeConst
    VkPresentModeKHR Swapchain::choose_present_mode(VkPhysicalDevice physical_device, VkSurfaceKHR surface) {
        // ReSharper restore CppParameterMayBeConst
        uint32_t count = 0;
        vkGetPhysicalDeviceSurfacePresentModesKHR(physical_device, surface, &count, nullptr);
        std::vector<VkPresentModeKHR> modes(count);
        vkGetPhysicalDeviceSurfacePresentModesKHR(physical_device, surface, &count, modes.data());

        for (const auto mode: modes) {
            if (mode == VK_PRESENT_MODE_MAILBOX_KHR) {
                spdlog::info("Present mode: MAILBOX");
                return mode;
            }
        }

        spdlog::warn("Present mode: FIFO (MAILBOX not available)");
        return VK_PRESENT_MODE_FIFO_KHR;
    }

    // Uses currentExtent when defined; otherwise queries framebuffer size and clamps
    VkExtent2D Swapchain::choose_extent(
        const VkSurfaceCapabilitiesKHR &capabilities, GLFWwindow *window) {
        if (capabilities.currentExtent.width != std::numeric_limits<uint32_t>::max()) {
            return capabilities.currentExtent;
        }

        spdlog::warn("Surface did not provide a fixed extent, querying framebuffer size");

        int width, height;
        glfwGetFramebufferSize(window, &width, &height);

        return {
            std::clamp(
                static_cast<uint32_t>(width),
                capabilities.minImageExtent.width,
                capabilities.maxImageExtent.width
            ),
            std::clamp(
                static_cast<uint32_t>(height),
                capabilities.minImageExtent.height,
                capabilities.maxImageExtent.height
            ),
        };
    }

    void Swapchain::create_image_views(VkDevice device) {
        // TODO: image view creation (task item after swapchain creation)
    }
} // namespace himalaya::rhi
