#pragma once

/**
 * @file swapchain.h
 * @brief Vulkan swapchain: presentation surface, images, and image views.
 */

#include <vector>

#include <vulkan/vulkan.h>

struct GLFWwindow;

namespace himalaya::rhi {
    class Context;

    /**
     * @brief Manages a Vulkan swapchain and its associated image views.
     *
     * Owns the VkSwapchainKHR, and creates one VkImageView per swapchain image.
     * The VkImage handles are owned by the swapchain itself (not manually destroyed).
     * Lifetime is managed explicitly via init() and destroy().
     */
    class Swapchain {
    public:
        /**
         * @brief Creates the swapchain and its image views.
         * @param context Vulkan context providing device, physical device, and surface.
         * @param window  GLFW window used to query framebuffer size for extent.
         */
        void init(const Context &context, GLFWwindow *window);

        /**
         * @brief Destroys image views and the swapchain.
         * @param device Logical device that owns the swapchain.
         */
        void destroy(VkDevice device) const;

        /** @brief Swapchain handle. */
        VkSwapchainKHR swapchain = VK_NULL_HANDLE;

        /** @brief Swapchain image format (e.g. B8G8R8A8_SRGB). */
        VkFormat format = VK_FORMAT_UNDEFINED;

        /** @brief Swapchain image extent in pixels. */
        VkExtent2D extent = {0, 0};

        /** @brief Swapchain images (owned by the swapchain, not manually destroyed). */
        std::vector<VkImage> images;

        /** @brief One image view per swapchain image. */
        std::vector<VkImageView> image_views;

    private:
        /**
         * @brief Selects the best surface format.
         *
         * Prefers B8G8R8A8_SRGB with SRGB_NONLINEAR color space.
         * Falls back to the first available format.
         */
        static VkSurfaceFormatKHR choose_surface_format(VkPhysicalDevice physical_device, VkSurfaceKHR surface);

        /**
         * @brief Selects the best present mode.
         *
         * Prefers MAILBOX (triple-buffered, no tearing, low latency).
         * Falls back to FIFO (guaranteed available, v-sync).
         */
        static VkPresentModeKHR choose_present_mode(VkPhysicalDevice physical_device, VkSurfaceKHR surface);

        /**
         * @brief Determines the swapchain extent from surface capabilities.
         *
         * Uses the surface's currentExtent if defined, otherwise queries
         * the GLFW framebuffer size and clamps to the supported range.
         */
        static VkExtent2D choose_extent(const VkSurfaceCapabilitiesKHR &capabilities, GLFWwindow *window);

        /**
         * @brief Creates a VkImageView for each swapchain image.
         */
        void create_image_views(VkDevice device);
    };
} // namespace himalaya::rhi
