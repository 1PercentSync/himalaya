/**
 * @file scene_loader.cpp
 * @brief glTF scene loading implementation.
 */

#include <himalaya/app/scene_loader.h>

#include <himalaya/rhi/descriptors.h>
#include <himalaya/rhi/resources.h>

#include <fastgltf/core.hpp>
#include <fastgltf/types.hpp>
#include <spdlog/spdlog.h>

#include <filesystem>

namespace himalaya::app {
    void SceneLoader::load(const std::string &path,
                           rhi::ResourceManager &resource_manager,
                           rhi::DescriptorManager &descriptor_manager,
                           framework::MaterialSystem &material_system,
                           const framework::DefaultTextures &default_textures,
                           const rhi::SamplerHandle default_sampler) {
        resource_manager_ = &resource_manager;
        descriptor_manager_ = &descriptor_manager;

        spdlog::info("Loading scene: {}", path);

        if (!std::filesystem::exists(path)) {
            spdlog::error("Scene file not found: {}", path);
            std::abort();
        }

        // Parse glTF
        auto data = fastgltf::GltfDataBuffer::FromPath(path);
        if (data.error() != fastgltf::Error::None) {
            spdlog::error("Failed to read glTF file: {}", path);
            std::abort();
        }

        constexpr auto options = fastgltf::Options::LoadExternalBuffers
                                 | fastgltf::Options::LoadExternalImages;

        fastgltf::Parser parser;
        auto asset = parser.loadGltf(data.get(),
                                     std::filesystem::path(path).parent_path(),
                                     options);
        if (asset.error() != fastgltf::Error::None) {
            spdlog::error("Failed to parse glTF '{}' (error {})",
                          path, static_cast<int>(asset.error()));
            std::abort();
        }

        spdlog::info("glTF parsed: {} meshes, {} materials, {} textures, {} nodes",
                     asset->meshes.size(),
                     asset->materials.size(),
                     asset->textures.size(),
                     asset->nodes.size());
    }

    void SceneLoader::destroy() {
        if (!resource_manager_) return;

        // Unregister bindless textures first (before destroying images)
        for (const auto idx: bindless_indices_) {
            descriptor_manager_->unregister_texture(idx);
        }
        bindless_indices_.clear();

        // Destroy texture images
        for (const auto handle: images_) {
            resource_manager_->destroy_image(handle);
        }
        images_.clear();

        // Destroy samplers
        for (const auto handle: samplers_) {
            resource_manager_->destroy_sampler(handle);
        }
        samplers_.clear();

        // Destroy vertex and index buffers
        for (const auto handle: buffers_) {
            resource_manager_->destroy_buffer(handle);
        }
        buffers_.clear();

        // Clear scene data
        meshes_.clear();
        material_instances_.clear();
        mesh_instances_.clear();
        directional_lights_.clear();

        resource_manager_ = nullptr;
        descriptor_manager_ = nullptr;
    }

    std::span<const framework::Mesh> SceneLoader::meshes() const {
        return meshes_;
    }

    std::span<const framework::MaterialInstance> SceneLoader::material_instances() const {
        return material_instances_;
    }

    std::span<const framework::MeshInstance> SceneLoader::mesh_instances() const {
        return mesh_instances_;
    }

    std::span<const framework::DirectionalLight> SceneLoader::directional_lights() const {
        return directional_lights_;
    }
} // namespace himalaya::app
