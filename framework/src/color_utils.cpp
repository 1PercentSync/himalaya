/**
 * @file color_utils.cpp
 * @brief Color science utilities implementation.
 */

#include <himalaya/framework/color_utils.h>

#include <algorithm>
#include <cmath>

namespace himalaya::framework {
    glm::vec3 color_temperature_to_rgb(const float kelvin) {
        // Clamp to supported range
        const float t = std::clamp(kelvin, 2000.0f, 12000.0f) / 100.0f;

        // Tanner Helland approximation (sRGB, then we keep in linear)
        // Red
        float r;
        if (t <= 66.0f) {
            r = 255.0f;
        } else {
            r = 329.698727446f * std::pow(t - 60.0f, -0.1332047592f);
            r = std::clamp(r, 0.0f, 255.0f);
        }

        // Green
        float g;
        if (t <= 66.0f) {
            g = 99.4708025861f * std::log(t) - 161.1195681661f;
            g = std::clamp(g, 0.0f, 255.0f);
        } else {
            g = 288.1221695283f * std::pow(t - 60.0f, -0.0755148492f);
            g = std::clamp(g, 0.0f, 255.0f);
        }

        // Blue
        float b;
        if (t >= 66.0f) {
            b = 255.0f;
        } else if (t <= 19.0f) {
            b = 0.0f;
        } else {
            b = 138.5177312231f * std::log(t - 10.0f) - 305.0447927307f;
            b = std::clamp(b, 0.0f, 255.0f);
        }

        // Convert from 0-255 sRGB to linear [0,1]
        // sRGB → linear: ((v/255)^2.2) is a common approximation
        const auto to_linear = [](const float v) {
            const float s = v / 255.0f;
            return std::pow(s, 2.2f);
        };

        const glm::vec3 linear{to_linear(r), to_linear(g), to_linear(b)};

        // Normalize so that 6500K ≈ (1, 1, 1)
        static const glm::vec3 ref = [&to_linear] {
            constexpr float t_ref = 6500.0f / 100.0f;
            constexpr float r_ref = 255.0f;
            const float g_ref = std::clamp(99.4708025861f * std::log(t_ref) - 161.1195681661f, 0.0f, 255.0f);
            const float b_ref = std::clamp(138.5177312231f * std::log(t_ref - 10.0f) - 305.0447927307f, 0.0f, 255.0f);
            return glm::vec3{to_linear(r_ref), to_linear(g_ref), to_linear(b_ref)};
        }();

        return linear / ref;
    }
} // namespace himalaya::framework
