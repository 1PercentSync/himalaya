#pragma once

/**
 * @file color_utils.h
 * @brief Color science utilities for the rendering framework.
 */

#include <glm/vec3.hpp>

namespace himalaya::framework {
    /**
     * @brief Converts color temperature (Kelvin) to linear-space RGB.
     *
     * Uses a piecewise polynomial approximation based on CIE color matching
     * functions. Normalized so that 6500K (D65 illuminant) produces
     * approximately (1, 1, 1).
     *
     * @param kelvin Color temperature in Kelvin, clamped to [2000, 12000].
     * @return Linear-space RGB color (components may slightly exceed 1.0
     *         at extreme temperatures).
     */
    glm::vec3 color_temperature_to_rgb(float kelvin);
} // namespace himalaya::framework
