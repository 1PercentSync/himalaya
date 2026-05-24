/**
 * @file gs_gpu_data.cpp
 * @brief GsGpuData implementation — GPU buffer allocation and GS scene upload.
 */

#include <himalaya/framework/gs_gpu_data.h>
#include <himalaya/rhi/resources.h>

#include <string>

#include <spdlog/spdlog.h>

namespace himalaya::framework {
    // ---- Cumulative SH coefficient counts per max SH degree ----
    // degree 0: 1 group (sh_coefs_0)
    // degree 1: 1 + 3 = 4 groups
    // degree 2: 1 + 3 + 5 = 9 groups
    // degree 3: 1 + 3 + 5 + 7 = 16 groups
    static constexpr uint32_t kShCoefCounts[] = {1, 4, 9, 16};

    // ---- Lifetime ----

    void GsGpuData::init(rhi::ResourceManager *rm) {
        rm_ = rm;
    }

    void GsGpuData::destroy() {
        if (core_buffer_.valid()) {
            rm_->destroy_buffer(core_buffer_);
            core_buffer_ = {};
        }

        for (auto &buf : sh_buffers_) {
            if (buf.valid()) {
                rm_->destroy_buffer(buf);
                buf = {};
            }
        }

        sh_groups_.clear();
        total_splat_count_ = 0;

        spdlog::info("GsGpuData: destroyed all GPU buffers");
    }

    // ---- Upload ----

    void GsGpuData::upload(const GaussianSplatScene &scene) {
        // Clean up any previous upload
        destroy();

        if (scene.primitives.empty()) {
            spdlog::info("GsGpuData: no GS primitives to upload");
            return;
        }

        // Group primitives by max SH degree. Primitives with the same
        // max_sh_degree are placed contiguously in the core buffer so
        // the projection pass can dispatch per-degree groups.
        std::vector<const GaussianSplatPrimitive *> degree_primitives[4];
        for (const auto &prim : scene.primitives) {
            const auto degree = prim.metadata.max_sh_degree;
            if (degree > 3) {
                spdlog::warn("GsGpuData: primitive has max_sh_degree={} (>3), clamping to 3", degree);
                degree_primitives[3].push_back(&prim);
            } else {
                degree_primitives[degree].push_back(&prim);
            }
        }

        // ---- Build merged core attributes array ----
        // Positions are transformed from local to world space using each
        // primitive's node transform. Splats are ordered by max_sh_degree
        // so each ShGroup maps to a contiguous range.

        std::vector<GaussianSplatCore> combined_cores;
        sh_groups_.clear();

        for (uint32_t degree = 0; degree < 4; ++degree) {
            if (degree_primitives[degree].empty()) {
                continue;
            }

            ShGroup group{};
            group.splat_offset = static_cast<uint32_t>(combined_cores.size());
            group.sh_degree = degree;

            for (const auto *prim : degree_primitives[degree]) {
                for (const auto &core : prim->cores) {
                    GaussianSplatCore transformed = core;
                    // Apply node transform to position (local → world space)
                    const glm::vec4 world_pos = prim->transform * glm::vec4(core.position, 1.0f);
                    transformed.position = glm::vec3(world_pos);
                    combined_cores.push_back(transformed);
                }
            }

            group.splat_count = static_cast<uint32_t>(combined_cores.size()) - group.splat_offset;
            sh_groups_.push_back(group);
        }

        total_splat_count_ = static_cast<uint32_t>(combined_cores.size());

        // ---- Create and upload core SSBO ----
        {
            const auto core_size = static_cast<uint64_t>(combined_cores.size()) * sizeof(GaussianSplatCore);
            core_buffer_ = rm_->create_buffer({
                .size = core_size,
                .usage = rhi::BufferUsage::StorageBuffer | rhi::BufferUsage::TransferDst,
                .memory = rhi::MemoryUsage::GpuOnly,
            }, "GS Core SSBO");
            rm_->upload_buffer(core_buffer_, combined_cores.data(), core_size);

            spdlog::info("GsGpuData: uploaded core SSBO ({} splats, {:.1f} KB)",
                          combined_cores.size(),
                          static_cast<double>(core_size) / 1024.0);
        }

        // ---- Build and upload per-degree SH SSBOs ----
        for (const auto &group : sh_groups_) {
            const uint32_t degree = group.sh_degree;
            const uint32_t coef_count = kShCoefCounts[degree]; // Cumulative coef groups per splat
            const uint32_t stride = coef_count;                // In vec3 units
            const uint64_t buffer_size = static_cast<uint64_t>(group.splat_count) * stride * sizeof(glm::vec3);

            // Interleave SH coefficients from all primitives in this degree group.
            // Per-splat layout: sh_coefs_0, then sh_coefs_1[0..2], then sh_coefs_2[0..4], etc.
            std::vector<glm::vec3> sh_data(group.splat_count * stride);
            uint32_t dst_splat = 0;

            for (const auto *prim : degree_primitives[degree]) {
                for (size_t i = 0; i < prim->cores.size(); ++i) {
                    const uint32_t base = dst_splat * stride;

                    // Degree 0 (always present)
                    sh_data[base + 0] = prim->sh_coefs_0[i];

                    if (degree >= 1) {
                        sh_data[base + 1] = prim->sh_coefs_1[0][i];
                        sh_data[base + 2] = prim->sh_coefs_1[1][i];
                        sh_data[base + 3] = prim->sh_coefs_1[2][i];
                    }
                    if (degree >= 2) {
                        sh_data[base + 4] = prim->sh_coefs_2[0][i];
                        sh_data[base + 5] = prim->sh_coefs_2[1][i];
                        sh_data[base + 6] = prim->sh_coefs_2[2][i];
                        sh_data[base + 7] = prim->sh_coefs_2[3][i];
                        sh_data[base + 8] = prim->sh_coefs_2[4][i];
                    }
                    if (degree >= 3) {
                        sh_data[base + 9] = prim->sh_coefs_3[0][i];
                        sh_data[base + 10] = prim->sh_coefs_3[1][i];
                        sh_data[base + 11] = prim->sh_coefs_3[2][i];
                        sh_data[base + 12] = prim->sh_coefs_3[3][i];
                        sh_data[base + 13] = prim->sh_coefs_3[4][i];
                        sh_data[base + 14] = prim->sh_coefs_3[5][i];
                        sh_data[base + 15] = prim->sh_coefs_3[6][i];
                    }

                    ++dst_splat;
                }
            }

            const auto name = "GS SH Degree " + std::to_string(degree) + " SSBO";
            sh_buffers_[degree] = rm_->create_buffer({
                .size = buffer_size,
                .usage = rhi::BufferUsage::StorageBuffer | rhi::BufferUsage::TransferDst,
                .memory = rhi::MemoryUsage::GpuOnly,
            }, name.c_str());
            rm_->upload_buffer(sh_buffers_[degree], sh_data.data(), buffer_size);

            spdlog::info("GsGpuData: uploaded SH degree {} SSBO ({} splats, {:.1f} KB)",
                          degree, group.splat_count,
                          static_cast<double>(buffer_size) / 1024.0);
        }
    }

    // ---- Accessors ----

    rhi::BufferHandle GsGpuData::core_buffer() const {
        return core_buffer_;
    }

    rhi::BufferHandle GsGpuData::sh_buffer(const uint32_t max_sh_degree) const {
        if (max_sh_degree > 3) {
            return {};
        }
        return sh_buffers_[max_sh_degree];
    }

    const std::vector<GsGpuData::ShGroup> &GsGpuData::sh_groups() const {
        return sh_groups_;
    }

    uint32_t GsGpuData::total_splat_count() const {
        return total_splat_count_;
    }
} // namespace himalaya::framework
