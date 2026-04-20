/**
 * @file lightmap_uv.cpp
 * @brief Lightmap UV generation via xatlas.
 */

#include <himalaya/framework/lightmap_uv.h>

#include <himalaya/xatlas/xatlas.h>

#include <spdlog/spdlog.h>

#include <cassert>

namespace himalaya::framework {
    LightmapUVResult generate_lightmap_uv(const std::span<const Vertex> vertices,
                                          const std::span<const uint32_t> indices,
                                          const uint32_t pack_resolution) {
        assert(!vertices.empty() && !indices.empty());
        assert(pack_resolution > 0 && (pack_resolution % 4) == 0);

        xatlas::Atlas *atlas = xatlas::Create();

        xatlas::MeshDecl decl;
        decl.vertexPositionData = &vertices[0].position;
        decl.vertexPositionStride = sizeof(Vertex);
        decl.vertexCount = static_cast<uint32_t>(vertices.size());
        decl.indexData = indices.data();
        decl.indexCount = static_cast<uint32_t>(indices.size());
        decl.indexFormat = xatlas::IndexFormat::UInt32;

        const xatlas::AddMeshError error = xatlas::AddMesh(atlas, decl);
        if (error != xatlas::AddMeshError::Success) {
            spdlog::error("lightmap_uv: xatlas AddMesh failed: {}", xatlas::StringForEnum(error));
            xatlas::Destroy(atlas);

            LightmapUVResult fallback;
            fallback.is_fallback = true;
            fallback.lightmap_uvs.resize(vertices.size(), glm::vec2(0.0f));
            fallback.new_indices.assign(indices.begin(), indices.end());
            fallback.vertex_remap.resize(vertices.size());
            for (uint32_t i = 0; i < vertices.size(); ++i) {
                fallback.vertex_remap[i] = i;
            }
            return fallback;
        }

        xatlas::ChartOptions chart_options;
        xatlas::PackOptions pack_options;
        pack_options.padding = 2;
        pack_options.resolution = pack_resolution;

        if constexpr (kDefaultLightmapUVQuality == LightmapUVQuality::Production) {
            chart_options.maxIterations = 4;
            pack_options.bruteForce = true;
        } else {
            chart_options.maxIterations = 1;
            pack_options.bruteForce = false;
        }

        xatlas::Generate(atlas, chart_options, pack_options);

        const xatlas::Mesh &out = atlas->meshes[0];

        LightmapUVResult result;

        if (atlas->width == 0 || atlas->height == 0) {
            spdlog::warn("lightmap_uv: xatlas produced 0x0 atlas (pack_resolution={})",
                         pack_resolution);
            result.is_fallback = true;
        }

        const float inv_w = (atlas->width > 0) ? 1.0f / static_cast<float>(atlas->width) : 0.0f;
        const float inv_h = (atlas->height > 0) ? 1.0f / static_cast<float>(atlas->height) : 0.0f;

        result.lightmap_uvs.resize(out.vertexCount);
        result.vertex_remap.resize(out.vertexCount);
        for (uint32_t i = 0; i < out.vertexCount; ++i) {
            result.lightmap_uvs[i] = {
                out.vertexArray[i].uv[0] * inv_w,
                out.vertexArray[i].uv[1] * inv_h
            };
            result.vertex_remap[i] = out.vertexArray[i].xref;
        }

        result.new_indices.assign(out.indexArray, out.indexArray + out.indexCount);

        spdlog::info("lightmap_uv: xatlas generated {}x{} atlas, {} -> {} vertices, {} indices",
                     atlas->width, atlas->height,
                     vertices.size(), out.vertexCount, out.indexCount);

        xatlas::Destroy(atlas);

        return result;
    }
} // namespace himalaya::framework
