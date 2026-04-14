/**
 * @file lightmap_uv.cpp
 * @brief Lightmap UV generation via xatlas with binary disk cache.
 */

#include <himalaya/framework/lightmap_uv.h>
#include <himalaya/framework/cache.h>

#include <himalaya/xatlas/xatlas.h>

#include <spdlog/spdlog.h>

#include <cassert>
#include <fstream>

namespace himalaya::framework {
    /// Cache file header (8 bytes): array counts only.
    /// Validation relies on file size consistency (like IBL alias table cache).
    struct CacheHeader {
        uint32_t vertex_count;
        uint32_t index_count;
    };

    static_assert(sizeof(CacheHeader) == 8);

    // --- Cache read/write helpers ---

    /** @brief Cache category for the current build configuration (debug/release isolation). */
#ifdef NDEBUG
    static constexpr std::string_view kCacheCategory = "lightmap_uv_release";
#else
    static constexpr std::string_view kCacheCategory = "lightmap_uv_debug";
#endif

    /**
     * Attempts to load a LightmapUVResult from disk cache.
     * Returns nullopt on cache miss or any read/validation failure.
     */
    static std::optional<LightmapUVResult> read_cache(const std::string &mesh_hash) {
        const auto path = cache_path(kCacheCategory, mesh_hash, ".bin");
        std::ifstream ifs(path, std::ios::binary);
        if (!ifs) {
            return std::nullopt;
        }

        CacheHeader header{};
        ifs.read(reinterpret_cast<char *>(&header), sizeof(header));
        if (!ifs) {
            return std::nullopt;
        }

        // Validate file size: header + uvs + indices + remap
        const auto expected_size = sizeof(CacheHeader)
                                   + header.vertex_count * sizeof(glm::vec2)
                                   + header.index_count * sizeof(uint32_t)
                                   + header.vertex_count * sizeof(uint32_t);
        ifs.seekg(0, std::ios::end);
        if (static_cast<uint64_t>(ifs.tellg()) != expected_size) {
            return std::nullopt;
        }
        ifs.seekg(sizeof(CacheHeader));

        LightmapUVResult result;
        result.lightmap_uvs.resize(header.vertex_count);
        result.new_indices.resize(header.index_count);
        result.vertex_remap.resize(header.vertex_count);

        ifs.read(reinterpret_cast<char *>(result.lightmap_uvs.data()),
                 static_cast<std::streamsize>(header.vertex_count * sizeof(glm::vec2)));
        ifs.read(reinterpret_cast<char *>(result.new_indices.data()),
                 static_cast<std::streamsize>(header.index_count * sizeof(uint32_t)));
        ifs.read(reinterpret_cast<char *>(result.vertex_remap.data()),
                 static_cast<std::streamsize>(header.vertex_count * sizeof(uint32_t)));

        if (!ifs) {
            return std::nullopt;
        }

        return result;
    }

    /**
     * Writes a LightmapUVResult to disk cache (best-effort, failure is non-fatal).
     */
    static void write_cache(const std::string &mesh_hash, const LightmapUVResult &result) {
        const auto path = cache_path(kCacheCategory, mesh_hash, ".bin");
        auto tmp_path = path;
        tmp_path += ".tmp";

        std::ofstream ofs(tmp_path, std::ios::binary | std::ios::trunc);
        if (!ofs) {
            spdlog::warn("lightmap_uv: failed to write cache {}", path.string());
            return;
        }

        CacheHeader header{};
        header.vertex_count = static_cast<uint32_t>(result.lightmap_uvs.size());
        header.index_count = static_cast<uint32_t>(result.new_indices.size());

        ofs.write(reinterpret_cast<const char *>(&header), sizeof(header));
        ofs.write(reinterpret_cast<const char *>(result.lightmap_uvs.data()),
                  static_cast<std::streamsize>(result.lightmap_uvs.size() * sizeof(glm::vec2)));
        ofs.write(reinterpret_cast<const char *>(result.new_indices.data()),
                  static_cast<std::streamsize>(result.new_indices.size() * sizeof(uint32_t)));
        ofs.write(reinterpret_cast<const char *>(result.vertex_remap.data()),
                  static_cast<std::streamsize>(result.vertex_remap.size() * sizeof(uint32_t)));
        ofs.close();
        if (!ofs.good()) {
            std::error_code ec;
            std::filesystem::remove(tmp_path, ec);
            return;
        }

        // Atomic rename (write-to-temp + rename, same as ktx2.cpp)
        std::error_code ec;
        std::filesystem::rename(tmp_path, path, ec);
        if (ec) {
            spdlog::warn("lightmap_uv: rename failed: {}", ec.message());
            std::filesystem::remove(tmp_path, ec);
        }
    }

    // --- Public API ---

    LightmapUVResult generate_lightmap_uv(const std::span<const Vertex> vertices,
                                          const std::span<const uint32_t> indices,
                                          const std::string &mesh_hash) {
        assert(!vertices.empty() && !indices.empty());

        // Try cache first
        if (auto cached = read_cache(mesh_hash)) {
            spdlog::info("lightmap_uv: cache hit for mesh {:.8}", mesh_hash);
            return std::move(*cached);
        }

        // --- Run xatlas ---
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
            // Cache and return degenerate result — identity remap, zero UVs
            LightmapUVResult fallback;
            fallback.lightmap_uvs.resize(vertices.size(), glm::vec2(0.0f));
            fallback.new_indices.assign(indices.begin(), indices.end());
            fallback.vertex_remap.resize(vertices.size());
            for (uint32_t i = 0; i < vertices.size(); ++i) {
                fallback.vertex_remap[i] = i;
            }
            write_cache(mesh_hash, fallback);
            return fallback;
        }

        xatlas::ChartOptions chart_options;
        xatlas::PackOptions pack_options;
        pack_options.padding = 2; // Gutter: 2 texel padding around UV islands

        if constexpr (kDefaultLightmapUVQuality == LightmapUVQuality::Production) {
            chart_options.maxIterations = 4;
            pack_options.bruteForce = true;
        } else {
            chart_options.maxIterations = 1;
            pack_options.bruteForce = false;
        }

        xatlas::Generate(atlas, chart_options, pack_options);

        // Extract results from meshes[0] (single mesh input)
        const xatlas::Mesh &out = atlas->meshes[0];

        LightmapUVResult result;

        // Normalize UVs from [0, atlas_size] to [0, 1]
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

        // Write cache (best-effort)
        write_cache(mesh_hash, result);

        return result;
    }
} // namespace himalaya::framework
