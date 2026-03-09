#include <himalaya/framework/mesh.h>

#include <mikktspace.h>

namespace himalaya::framework {
    // MikkTSpace operates on a mesh described by callbacks. This struct bundles
    // the vertex and index spans so callbacks can access them via pUserData.
    namespace {
        struct MikkUserData {
            std::span<Vertex> vertices;
            std::span<const uint32_t> indices;
        };

        int mikk_get_num_faces(const SMikkTSpaceContext *ctx) {
            const auto *data = static_cast<const MikkUserData *>(ctx->m_pUserData);
            return static_cast<int>(data->indices.size() / 3);
        }

        int mikk_get_num_vertices_of_face(const SMikkTSpaceContext *, int) {
            return 3; // triangles only
        }

        void mikk_get_position(const SMikkTSpaceContext *ctx, float out[], const int face, const int vert) {
            const auto *data = static_cast<const MikkUserData *>(ctx->m_pUserData);
            const auto &pos = data->vertices[data->indices[face * 3 + vert]].position;
            out[0] = pos.x;
            out[1] = pos.y;
            out[2] = pos.z;
        }

        void mikk_get_normal(const SMikkTSpaceContext *ctx, float out[], const int face, const int vert) {
            const auto *data = static_cast<const MikkUserData *>(ctx->m_pUserData);
            const auto &n = data->vertices[data->indices[face * 3 + vert]].normal;
            out[0] = n.x;
            out[1] = n.y;
            out[2] = n.z;
        }

        void mikk_get_tex_coord(const SMikkTSpaceContext *ctx, float out[], const int face, const int vert) {
            const auto *data = static_cast<const MikkUserData *>(ctx->m_pUserData);
            const auto &uv = data->vertices[data->indices[face * 3 + vert]].uv0;
            out[0] = uv.x;
            out[1] = uv.y;
        }

        void mikk_set_tspace_basic(const SMikkTSpaceContext *ctx,
                                   const float tangent[],
                                   const float sign,
                                   const int face,
                                   const int vert) {
            auto *data = static_cast<MikkUserData *>(ctx->m_pUserData);
            auto &t = data->vertices[data->indices[face * 3 + vert]].tangent;
            t.x = tangent[0];
            t.y = tangent[1];
            t.z = tangent[2];
            t.w = sign;
        }
    } // anonymous namespace
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

    void generate_tangents(const std::span<Vertex> vertices, const std::span<const uint32_t> indices) {
        MikkUserData user_data{vertices, indices};

        SMikkTSpaceInterface iface{};
        iface.m_getNumFaces = mikk_get_num_faces;
        iface.m_getNumVerticesOfFace = mikk_get_num_vertices_of_face;
        iface.m_getPosition = mikk_get_position;
        iface.m_getNormal = mikk_get_normal;
        iface.m_getTexCoord = mikk_get_tex_coord;
        iface.m_setTSpaceBasic = mikk_set_tspace_basic;

        SMikkTSpaceContext ctx{};
        ctx.m_pInterface = &iface;
        ctx.m_pUserData = &user_data;

        genTangSpaceDefault(&ctx);
    }
} // namespace himalaya::framework
