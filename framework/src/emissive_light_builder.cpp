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

#include <spdlog/spdlog.h>

namespace himalaya::framework {
    void EmissiveLightBuilder::build(rhi::Context &ctx,
                                     rhi::ResourceManager &rm,
                                     const std::span<const Mesh> meshes,
                                     const std::span<const MeshInstance> instances,
                                     const std::span<const MaterialInstance> materials) {
        // Auto-destroy previous resources (same pattern as SceneASBuilder)
        if (triangle_buffer_.valid() || alias_table_buffer_.valid()) {
            destroy();
        }

        resource_manager_ = &rm;
        emissive_count_ = 0;

        // Implementation filled in by subsequent task items:
        // - Emissive triangle collection (world-space vertices/UV/area)
        // - Power-weighted alias table construction (Vose's algorithm)
        // - EmissiveTriangleBuffer SSBO upload
        // - EmissiveAliasTable SSBO upload
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
