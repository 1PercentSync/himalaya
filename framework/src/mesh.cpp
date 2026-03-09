#include <himalaya/framework/mesh.h>

namespace himalaya::framework {
    VkVertexInputBindingDescription Vertex::binding_description() {
        return {
            .binding = 0,
            .stride = sizeof(Vertex),
            .inputRate = VK_VERTEX_INPUT_RATE_VERTEX,
        };
    }

    std::array<VkVertexInputAttributeDescription, 5> Vertex::attribute_descriptions() {
        return {
            {
                {
                    .location = 0, .binding = 0, .format = VK_FORMAT_R32G32B32_SFLOAT,
                    .offset = offsetof(Vertex, position)
                },
                {
                    .location = 1, .binding = 0, .format = VK_FORMAT_R32G32B32_SFLOAT,
                    .offset = offsetof(Vertex, normal)
                },
                {
                    .location = 2, .binding = 0, .format = VK_FORMAT_R32G32_SFLOAT,
                    .offset = offsetof(Vertex, uv0)
                },
                {
                    .location = 3, .binding = 0, .format = VK_FORMAT_R32G32B32A32_SFLOAT,
                    .offset = offsetof(Vertex, tangent)
                },
                {
                    .location = 4, .binding = 0, .format = VK_FORMAT_R32G32_SFLOAT,
                    .offset = offsetof(Vertex, uv1)
                },
            }
        };
    }
} // namespace himalaya::framework
