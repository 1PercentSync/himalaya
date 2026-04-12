#pragma once

/**
 * @file swapchain.h
 * @brief Vulkan swapchain: presentation surface, images, and image views.
 */

#include <cstdint>
#include <vector>

#include <vulkan/vulkan.h>

struct GLFWwindow;

namespace himalaya::rhi {
    class Context;

    /** @brief Swapchain present mode selection. */
    enum class PresentMode : uint8_t {
        Fifo,       ///< VSync — wait for vertical blank (VK_PRESENT_MODE_FIFO_KHR, guaranteed)
        Mailbox,    ///< Triple-buffered, no tearing, low latency (VK_PRESENT_MODE_MAILBOX_KHR)
        Immediate,  ///< No sync, allows tearing, uncapped framerate (VK_PRESENT_MODE_IMMEDIATE_KHR)
    };

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
         * @param mode    Present mode selection (default Mailbox).
         */
        void init(const Context &context, GLFWwindow *window, PresentMode mode = PresentMode::Mailbox);

        /**
         * @brief Recreates the swapchain after a resize or suboptimal present.
         *
         * Waits for the device to be idle, destroys the old resources,
         * creates a new swapchain (passing the old handle for driver recycling),
         * then destroys the old swapchain.
         *
         * @param context Vulkan context providing device, physical device, and surface.
         * @param window  GLFW window used to query the new framebuffer size.
         */
        void recreate(const Context &context, GLFWwindow *window);

        /**
         * @brief Destroys image views and the swapchain.
         * @param device Logical device that owns the swapchain.
         */
        void destroy(VkDevice device) const;

        /** @brief Current present mode. Read by recreate() to preserve the setting. */
        PresentMode present_mode = PresentMode::Mailbox;

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

        /**
         * @brief One render-finished semaphore per swapchain image.
         *
         * Indexed by the acquired image index (not the frame index).
         * The presentation engine holds the semaphore until the image is
         * actually displayed, so per-frame semaphores are insufficient
         * when the swapchain has more images than frames in flight.
         */
        std::vector<VkSemaphore> render_finished_semaphores;

    private:
        /**
         * @brief Selects the best surface format.
         *
         * Prefers B8G8R8A8_SRGB with SRGB_NONLINEAR color space.
         * Falls back to the first available format.
         */
        static VkSurfaceFormatKHR choose_surface_format(VkPhysicalDevice physical_device, VkSurfaceKHR surface);

        /**
         * @brief Selects the Vulkan present mode from the requested PresentMode.
         *
         * FIFO is always available. MAILBOX and IMMEDIATE fall back to FIFO
         * with a warning if unsupported by the surface.
         */
        static VkPresentModeKHR choose_present_mode(VkPhysicalDevice physical_device, VkSurfaceKHR surface, PresentMode mode);

        /**
         * @brief Determines the swapchain extent from surface capabilities.
         *
         * Uses the surface's currentExtent if defined, otherwise queries
         * the GLFW framebuffer size and clamps to the supported range.
         */
        static VkExtent2D choose_extent(const VkSurfaceCapabilitiesKHR &capabilities, GLFWwindow *window);

        /**
         * @brief Core creation logic shared by init() and recreate().
         *
         * Queries surface capabilities, selects format/present mode/extent,
         * creates the swapchain, retrieves images, and creates image views
         * and per-image semaphores.
         *
         * @param context       Vulkan context providing device, physical device, and surface.
         * @param window        GLFW window used to query framebuffer size for extent.
         * @param old_swapchain Previous swapchain handle for driver recycling,
         *                      or VK_NULL_HANDLE for first creation.
         */
        void create_resources(const Context &context, GLFWwindow *window, VkSwapchainKHR old_swapchain);

        /**
         * @brief Creates a VkImageView for each swapchain image.
         */
        void create_image_views(VkDevice device);
    };
} // namespace himalaya::rhi
