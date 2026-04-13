/**
 * @file probe_placement.cpp
 * @brief Automatic reflection probe placement — grid generation + RT filtering.
 */

#include <himalaya/framework/probe_placement.h>

#include <spdlog/spdlog.h>

#include <glm/vec4.hpp>

#include <cmath>

namespace himalaya::framework {

    ProbeGrid generate_probe_grid(
        [[maybe_unused]] rhi::Context &ctx,
        [[maybe_unused]] rhi::ResourceManager &rm,
        [[maybe_unused]] rhi::ShaderCompiler &sc,
        [[maybe_unused]] rhi::DescriptorManager &dm,
        const AABB &scene_bounds,
        float grid_spacing,
        [[maybe_unused]] uint32_t ray_count,
        [[maybe_unused]] float enclosure_threshold)
    {
        // ---- Phase 1: Generate uniform grid candidates ----

        const glm::vec3 extent = scene_bounds.max - scene_bounds.min;

        // Guard against degenerate AABB or invalid spacing
        if (grid_spacing <= 0.0f || extent.x < 0.0f || extent.y < 0.0f || extent.z < 0.0f) {
            spdlog::warn("Probe placement: invalid parameters (spacing={:.2f}, extent=[{:.1f}, {:.1f}, {:.1f}])",
                         grid_spacing, extent.x, extent.y, extent.z);
            return {};
        }

        // Grid dimensions: at least 1 sample per axis
        const auto grid_x = static_cast<uint32_t>(std::floor(extent.x / grid_spacing)) + 1;
        const auto grid_y = static_cast<uint32_t>(std::floor(extent.y / grid_spacing)) + 1;
        const auto grid_z = static_cast<uint32_t>(std::floor(extent.z / grid_spacing)) + 1;
        const uint32_t total = grid_x * grid_y * grid_z;

        // Candidate positions as vec4 (w=0, 16-byte aligned for GPU buffer)
        std::vector<glm::vec4> candidates;
        candidates.reserve(total);

        for (uint32_t iz = 0; iz < grid_z; ++iz) {
            for (uint32_t iy = 0; iy < grid_y; ++iy) {
                for (uint32_t ix = 0; ix < grid_x; ++ix) {
                    const glm::vec3 pos = scene_bounds.min + glm::vec3(
                        static_cast<float>(ix) * grid_spacing,
                        static_cast<float>(iy) * grid_spacing,
                        static_cast<float>(iz) * grid_spacing);
                    candidates.emplace_back(pos, 0.0f);
                }
            }
        }

        spdlog::info("Probe placement: {} candidates on {}x{}x{} grid (spacing {:.2f}m)",
                     candidates.size(), grid_x, grid_y, grid_z, grid_spacing);

        // ---- Phase 2: RT geometric filtering ----
        // Dispatches probe_filter.comp, reads back per-candidate pass/fail results.
        // Added in Step 10 item 4.

        // ---- Phase 3: Collect survivors ----

        ProbeGrid result;
        result.stats.candidate_count = static_cast<uint32_t>(candidates.size());
        result.stats.passed = result.stats.candidate_count;
        result.positions.reserve(candidates.size());

        for (const auto &c : candidates) {
            result.positions.emplace_back(c.x, c.y, c.z);
        }

        spdlog::info("Probe placement: {} passed / {} culled (Rule1={}, Rule2={})",
                     result.stats.passed,
                     result.stats.rule1_culled + result.stats.rule2_culled,
                     result.stats.rule1_culled, result.stats.rule2_culled);

        return result;
    }

} // namespace himalaya::framework
