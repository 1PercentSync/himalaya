#pragma once

/**
 * @file render_constants.h
 * @brief Centralized render target format constants.
 *
 * All render target formats are defined here so that passes reference a
 * single source of truth. These are hardcoded per project design decisions
 * (see CLAUDE.md: depth D32Sfloat, HDR R16G16B16A16F).
 */

#include <vulkan/vulkan.h>

namespace himalaya::framework {

/** Scene depth buffer format (reverse-Z, no stencil). */
constexpr VkFormat kDepthFormat = VK_FORMAT_D32_SFLOAT;

/** Main HDR color render target format. */
constexpr VkFormat kHdrColorFormat = VK_FORMAT_R16G16B16A16_SFLOAT;

/** G-buffer world-space normal format (10-bit per channel, pack32). */
constexpr VkFormat kNormalFormat = VK_FORMAT_A2B10G10R10_UNORM_PACK32;

/** G-buffer roughness format (single channel). */
constexpr VkFormat kRoughnessFormat = VK_FORMAT_R8_UNORM;

} // namespace himalaya::framework
