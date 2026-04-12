/**
 * @file emissive_light_builder.cpp
 * @brief EmissiveLightBuilder implementation.
 *
 * Scans scene meshes for emissive materials, collects world-space triangle
 * data, builds a power-weighted alias table (Vose's algorithm), and uploads
 * two SSBOs for RT shader NEE sampling.
 */

#include <himalaya/framework/emissive_light_builder.h>

#include <himalaya/rhi/context.h>
#include <himalaya/rhi/resources.h>

#include <glm/glm.hpp>
#include <spdlog/spdlog.h>

#include <cassert>

namespace himalaya::framework {
    namespace {
        /** @brief ITU-R BT.709 luminance from linear RGB. */
        float luminance(const glm::vec3 &c) {
            return 0.2126f * c.r + 0.7152f * c.g + 0.0722f * c.b;
        }
    } // namespace

    void EmissiveLightBuilder::build(rhi::Context &ctx,
                                     rhi::ResourceManager &rm,
                                     const std::span<const Mesh> meshes,
                                     const std::span<const MeshInstance> instances,
                                     const std::span<const GPUMaterialData> gpu_materials,
                                     const std::span<const std::vector<Vertex>> mesh_vertices,
                                     const std::span<const std::vector<uint32_t>> mesh_indices) {
        // Auto-destroy previous resources (same pattern as SceneASBuilder)
        if (triangle_buffer_.valid() || alias_table_buffer_.valid()) {
            destroy();
        }

        resource_manager_ = &rm;
        emissive_count_ = 0;

        assert(mesh_vertices.size() == meshes.size());
        assert(mesh_indices.size() == meshes.size());

        // ---- Phase 1: Collect emissive triangles ----
        // For each mesh instance, check material emissive_factor > 0.
        // Transform vertices to world space, compute triangle area,
        // and store per-triangle data for SSBO upload.

        std::vector<EmissiveTriangle> triangles;
        std::vector<float> powers; // luminance(emissive_factor) × area

        for (const auto &inst: instances) {
            const uint32_t mesh_id = inst.mesh_id;
            if (mesh_id >= meshes.size()) {
                continue;
            }

            const auto &mesh = meshes[mesh_id];
            if (mesh.material_id >= gpu_materials.size()) {
                continue;
            }

            const auto &mat = gpu_materials[mesh.material_id];
            const auto emissive_f = glm::vec3(mat.emissive_factor);

            // Skip non-emissive materials
            if (emissive_f.r <= 0.0f && emissive_f.g <= 0.0f && emissive_f.b <= 0.0f) {
                continue;
            }

            // Skip degenerate primitives
            const auto &verts = mesh_vertices[mesh_id];
            const auto &idx = mesh_indices[mesh_id];
            if (verts.empty() || idx.size() < 3) {
                continue;
            }

            const float emissive_lum = luminance(emissive_f);
            const glm::mat4 &xform = inst.transform;

            // Iterate triangles
            for (size_t i = 0; i + 2 < idx.size(); i += 3) {
                const uint32_t i0 = idx[i];
                const uint32_t i1 = idx[i + 1];
                const uint32_t i2 = idx[i + 2];

                if (i0 >= verts.size() || i1 >= verts.size() || i2 >= verts.size()) {
                    continue;
                }

                // Transform to world space
                const glm::vec3 v0 = glm::vec3(xform * glm::vec4(verts[i0].position, 1.0f));
                const glm::vec3 v1 = glm::vec3(xform * glm::vec4(verts[i1].position, 1.0f));
                const glm::vec3 v2 = glm::vec3(xform * glm::vec4(verts[i2].position, 1.0f));

                // Triangle area = 0.5 * ||cross(e1, e2)||
                const glm::vec3 e1 = v1 - v0;
                const glm::vec3 e2 = v2 - v0;
                const float area = 0.5f * glm::length(glm::cross(e1, e2));

                // Skip degenerate triangles
                if (area <= 0.0f) {
                    continue;
                }

                triangles.push_back({
                    .v0 = v0, ._pad0 = 0.0f,
                    .v1 = v1, ._pad1 = 0.0f,
                    .v2 = v2, ._pad2 = 0.0f,
                    .emission = emissive_f,
                    .area = area,
                    .material_index = mesh.material_id,
                    ._pad3 = 0,
                    .uv0 = verts[i0].uv0,
                    .uv1 = verts[i1].uv0,
                    .uv2 = verts[i2].uv0,
                });

                powers.push_back(emissive_lum * area);
            }
        }

        emissive_count_ = static_cast<uint32_t>(triangles.size());

        if (emissive_count_ == 0) {
            spdlog::info("EmissiveLightBuilder: no emissive triangles found");
            return;
        }

        spdlog::info("EmissiveLightBuilder: collected {} emissive triangles", emissive_count_);

        // ---- Phase 2: Power-weighted alias table (Vose's algorithm, O(N)) ----

        struct AliasEntry {
            float prob;
            uint32_t alias;
        };

        std::vector<AliasEntry> table(emissive_count_);

        // Sum total power for normalization
        double power_sum = 0.0;
        for (const float p : powers) {
            power_sum += static_cast<double>(p);
        }

        // Normalize weights so average = 1.0
        const double avg = power_sum / static_cast<double>(emissive_count_);
        std::vector<float> normalized(emissive_count_);
        for (uint32_t i = 0; i < emissive_count_; ++i) {
            normalized[i] = (avg > 0.0)
                ? static_cast<float>(static_cast<double>(powers[i]) / avg)
                : 1.0f;
        }

        // Partition into small (< 1) and large (>= 1) work lists
        std::vector<uint32_t> small, large;
        small.reserve(emissive_count_);
        large.reserve(emissive_count_);
        for (uint32_t i = 0; i < emissive_count_; ++i) {
            if (normalized[i] < 1.0f) {
                small.push_back(i);
            } else {
                large.push_back(i);
            }
        }

        // Build alias table
        while (!small.empty() && !large.empty()) {
            const uint32_t s = small.back();
            small.pop_back();
            const uint32_t l = large.back();
            large.pop_back();

            table[s].prob = normalized[s];
            table[s].alias = l;

            normalized[l] = (normalized[l] + normalized[s]) - 1.0f;

            if (normalized[l] < 1.0f) {
                small.push_back(l);
            } else {
                large.push_back(l);
            }
        }

        // Remaining entries get probability 1.0 (numerical cleanup)
        while (!large.empty()) {
            const uint32_t l = large.back();
            large.pop_back();
            table[l].prob = 1.0f;
            table[l].alias = l;
        }
        while (!small.empty()) {
            const uint32_t s = small.back();
            small.pop_back();
            table[s].prob = 1.0f;
            table[s].alias = s;
        }

        spdlog::info("EmissiveLightBuilder: alias table built (total power {:.2f})", power_sum);

        // TODO: Phase 3 — SSBO upload (subsequent task items)
    }

    void EmissiveLightBuilder::destroy() {
        if (resource_manager_) {
            if (triangle_buffer_.valid()) {
                resource_manager_->destroy_buffer(triangle_buffer_);
                triangle_buffer_ = {};
            }
            if (alias_table_buffer_.valid()) {
                resource_manager_->destroy_buffer(alias_table_buffer_);
                alias_table_buffer_ = {};
            }
        }
        emissive_count_ = 0;
    }

    uint32_t EmissiveLightBuilder::emissive_count() const {
        return emissive_count_;
    }

    rhi::BufferHandle EmissiveLightBuilder::triangle_buffer() const {
        return triangle_buffer_;
    }

    rhi::BufferHandle EmissiveLightBuilder::alias_table_buffer() const {
        return alias_table_buffer_;
    }
} // namespace himalaya::framework
