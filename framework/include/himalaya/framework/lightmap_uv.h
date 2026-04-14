#pragma once

/**
 * @file lightmap_uv.h
 * @brief Lightmap UV generation using xatlas, with disk caching.
 */

#include <himalaya/framework/mesh.h>

#include <glm/vec2.hpp>

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace himalaya::framework {
    /**
     * @brief xatlas quality preset controlling chart iteration count and pack strategy.
     *
     * Fast uses minimal iterations and greedy packing (seconds per mesh).
     * Production uses higher iterations and brute-force packing (best quality, cached).
     */
    enum class LightmapUVQuality : uint8_t {
        Fast,       ///< maxIterations=1, bruteForce=false (debug builds).
        Production, ///< maxIterations=4, bruteForce=true  (release builds).
    };

    /** @brief Compile-time default quality: Production in release, Fast in debug. */
#ifdef NDEBUG
    inline constexpr auto kDefaultLightmapUVQuality = LightmapUVQuality::Production;
#else
    inline constexpr auto kDefaultLightmapUVQuality = LightmapUVQuality::Fast;
#endif
    /**
     * @brief Per-mesh xatlas output (or loaded from cache).
     *
     * Contains the lightmap UV coordinates, remapped index buffer,
     * and vertex remap table produced by xatlas. The caller uses
     * vertex_remap to reconstruct the full vertex buffer (copying
     * position/normal/uv0/tangent from the original vertex at
     * vertex_remap[i], and writing lightmap_uvs[i] into uv1).
     */
    struct LightmapUVResult {
        /** @brief Per-vertex lightmap UV coordinates (normalized [0,1]). */
        std::vector<glm::vec2> lightmap_uvs;

        /** @brief New index buffer (topology may change due to vertex splitting). */
        std::vector<uint32_t> new_indices;

        /** @brief new_vertex -> original_vertex mapping. */
        std::vector<uint32_t> vertex_remap;

        /** @brief True if loaded from disk cache, false if xatlas ran. */
        bool cache_hit = false;

        /** @brief True if this is a degenerate fallback (xatlas failed or 0x0 atlas). */
        bool is_fallback = false;
    };

    /**
     * @brief Generates lightmap UV for a mesh using xatlas, or loads from cache.
     *
     * The caller is responsible for skipping this call when the mesh already
     * has TEXCOORD_1 (no topology change needed in that case).
     *
     * Cache key: mesh_hash (xxHash of vertex positions + indices, computed
     * by the caller). Cache hit: load from disk, skip xatlas. Cache miss:
     * run xatlas, save to disk.
     *
     * This is a pure CPU function — no GPU or RHI involvement.
     *
     * @param vertices   Source vertex array (only positions are used by xatlas).
     * @param indices    Source index array (uint32_t triangle list).
     * @param mesh_hash  Pre-computed content hash of positions + indices.
     * @return LightmapUVResult with generated lightmap UVs, new indices, and remap table.
     */
    [[nodiscard]] LightmapUVResult generate_lightmap_uv(std::span<const Vertex> vertices,
                                                        std::span<const uint32_t> indices,
                                                        const std::string &mesh_hash);
} // namespace himalaya::framework
