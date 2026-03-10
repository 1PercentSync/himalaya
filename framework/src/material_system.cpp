/**
 * @file material_system.cpp
 * @brief MaterialSystem implementation.
 *
 * The Material SSBO is a GpuOnly buffer uploaded once via staging during
 * scene loading. Descriptor write is delegated to DescriptorManager,
 * which writes binding 2 to both per-frame Set 0 instances.
 */

#include <himalaya/framework/material_system.h>
#include <himalaya/rhi/descriptors.h>
#include <himalaya/rhi/resources.h>

#include <cassert>
#include <spdlog/spdlog.h>

namespace himalaya::framework {
    void fill_material_defaults(GPUMaterialData &data,
                                const rhi::BindlessIndex default_white,
                                const rhi::BindlessIndex default_flat_normal,
                                const rhi::BindlessIndex default_black) {
        if (data.base_color_tex == UINT32_MAX) {
            data.base_color_tex = default_white.index;
        }
        if (data.emissive_tex == UINT32_MAX) {
            data.emissive_tex = default_black.index;
        }
        if (data.metallic_roughness_tex == UINT32_MAX) {
            data.metallic_roughness_tex = default_white.index;
        }
        if (data.normal_tex == UINT32_MAX) {
            data.normal_tex = default_flat_normal.index;
        }
        if (data.occlusion_tex == UINT32_MAX) {
            data.occlusion_tex = default_white.index;
        }
    }

    void MaterialSystem::init(rhi::ResourceManager *resource_manager, rhi::DescriptorManager *descriptor_manager) {
        resource_manager_ = resource_manager;
        descriptor_manager_ = descriptor_manager;
    }

    void MaterialSystem::destroy() {
        if (material_buffer_.valid()) {
            resource_manager_->destroy_buffer(material_buffer_);
            material_buffer_ = {};
        }
        material_count_ = 0;

        spdlog::info("MaterialSystem destroyed");
    }

    void MaterialSystem::upload_materials(const std::span<const GPUMaterialData> materials) {
        assert(!materials.empty());

        // Support scene switch: destroy previous buffer
        if (material_buffer_.valid()) {
            resource_manager_->destroy_buffer(material_buffer_);
            material_buffer_ = {};
        }

        material_count_ = static_cast<uint32_t>(materials.size());
        const auto buffer_size = static_cast<uint64_t>(material_count_) * sizeof(GPUMaterialData);

        // Create GpuOnly SSBO sized exactly for the loaded materials
        material_buffer_ = resource_manager_->create_buffer({
            .size = buffer_size,
            .usage = rhi::BufferUsage::StorageBuffer | rhi::BufferUsage::TransferDst,
            .memory = rhi::MemoryUsage::GpuOnly,
        });

        // Upload via staging buffer (caller must be in immediate scope)
        resource_manager_->upload_buffer(material_buffer_, materials.data(), buffer_size);

        // Write descriptor to both per-frame Set 0 (binding 2 = MaterialBuffer)
        descriptor_manager_->write_set0_buffer(2, material_buffer_, buffer_size);

        spdlog::info("MaterialSystem: uploaded {} materials ({} bytes)", material_count_, buffer_size);
    }

    rhi::BufferHandle MaterialSystem::get_buffer() const {
        return material_buffer_;
    }

    uint32_t MaterialSystem::material_count() const {
        return material_count_;
    }
} // namespace himalaya::framework
