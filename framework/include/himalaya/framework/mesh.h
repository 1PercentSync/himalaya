#pragma once

/**
 * @file mesh.h
 * @brief Unified vertex format and mesh data management.
 */

#include <glm/glm.hpp>
#include <vulkan/vulkan.h>

#include <array>

namespace himalaya::framework {
    /**
     * @brief Unified vertex format for all meshes.
     *
     * All meshes use this fixed layout regardless of source data.
     * Missing attributes are filled with sensible defaults at load time.
     */
    struct Vertex {
        /** @brief World-space position. */
        glm::vec3 position;

        /** @brief Surface normal (normalized). */
        glm::vec3 normal;

        /** @brief Primary texture coordinates. */
        glm::vec2 uv0;

        /** @brief Tangent vector with handedness in w (MikkTSpace convention). */
        glm::vec4 tangent;

        /** @brief Secondary texture coordinates (glTF TEXCOORD_1). */
        glm::vec2 uv1;

        /** @brief Returns the vertex input binding description (single binding 0, per-vertex). */
        [[nodiscard]] static VkVertexInputBindingDescription binding_description();

        /** @brief Returns the vertex input attribute descriptions for all 5 attributes. */
        [[nodiscard]] static std::array<VkVertexInputAttributeDescription, 5> attribute_descriptions();
    };
} // namespace himalaya::framework
