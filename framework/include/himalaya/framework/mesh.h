#pragma once

/**
 * @file mesh.h
 * @brief Unified vertex format and mesh data management.
 */

#include <himalaya/rhi/types.h>

#include <glm/glm.hpp>
#include <vulkan/vulkan.h>

#include <array>
#include <span>

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

    /**
     * @brief GPU-resident mesh data.
     *
     * Holds RHI buffer handles for vertex and index data, along with
     * counts needed for draw calls. Buffers are owned externally
     * (e.g. by SceneLoader) and destroyed via ResourceManager.
     */
    struct Mesh {
        /** @brief Handle to the GPU vertex buffer (Vertex[]). */
        rhi::BufferHandle vertex_buffer;

        /** @brief Handle to the GPU index buffer (uint32_t[]). */
        rhi::BufferHandle index_buffer;

        /** @brief Number of vertices in the vertex buffer. */
        uint32_t vertex_count = 0;

        /** @brief Number of indices in the index buffer. */
        uint32_t index_count = 0;
    };

    /**
     * @brief Generates tangent vectors for vertices using MikkTSpace.
     *
     * Overwrites the tangent field of each vertex. Requires valid position,
     * normal, and uv0 data. Indices define the triangle topology.
     */
    void generate_tangents(std::span<Vertex> vertices, std::span<const uint32_t> indices);
} // namespace himalaya::framework
