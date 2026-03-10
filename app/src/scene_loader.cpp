/**
 * @file scene_loader.cpp
 * @brief glTF scene loading implementation.
 */

#include <himalaya/app/scene_loader.h>

#include <himalaya/rhi/descriptors.h>
#include <himalaya/rhi/resources.h>

#include <fastgltf/core.hpp>
#include <fastgltf/tools.hpp>
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

        auto &gltf = asset.get();

        spdlog::info("glTF parsed: {} meshes, {} materials, {} textures, {} nodes",
                     gltf.meshes.size(),
                     gltf.materials.size(),
                     gltf.textures.size(),
                     gltf.nodes.size());

        // --- Load meshes (one Mesh per glTF primitive) ---
        for (const auto &gltf_mesh: gltf.meshes) {
            for (const auto &primitive: gltf_mesh.primitives) {
                // Position (required by glTF spec)
                const auto pos_it = primitive.findAttribute("POSITION");
                if (pos_it == primitive.attributes.end()) {
                    spdlog::error("Mesh '{}' primitive missing POSITION attribute",
                                  std::string(gltf_mesh.name));
                    std::abort();
                }
                const auto &pos_accessor = gltf.accessors[pos_it->accessorIndex];
                const auto vertex_count = pos_accessor.count;

                std::vector<framework::Vertex> vertices(vertex_count);

                {
                    size_t i = 0;
                    for (auto p: fastgltf::iterateAccessor<fastgltf::math::fvec3>(gltf, pos_accessor)) {
                        vertices[i].position = {p.x(), p.y(), p.z()};
                        ++i;
                    }
                }

                // Normal (optional, default +Z)
                bool has_normals = false;
                if (const auto it = primitive.findAttribute("NORMAL");
                    it != primitive.attributes.end()) {
                    has_normals = true;
                    const auto &accessor = gltf.accessors[it->accessorIndex];
                    size_t i = 0;
                    for (auto n: fastgltf::iterateAccessor<fastgltf::math::fvec3>(gltf, accessor)) {
                        vertices[i].normal = {n.x(), n.y(), n.z()};
                        ++i;
                    }
                } else {
                    for (auto &v: vertices) v.normal = {0.0f, 0.0f, 1.0f};
                }

                // TEXCOORD_0 (optional, zero-initialized default is fine)
                bool has_uv0 = false;
                if (const auto it = primitive.findAttribute("TEXCOORD_0");
                    it != primitive.attributes.end()) {
                    has_uv0 = true;
                    const auto &accessor = gltf.accessors[it->accessorIndex];
                    size_t i = 0;
                    for (auto uv: fastgltf::iterateAccessor<fastgltf::math::fvec2>(gltf, accessor)) {
                        vertices[i].uv0 = {uv.x(), uv.y()};
                        ++i;
                    }
                }

                // TANGENT (optional)
                bool has_tangent = false;
                if (const auto it = primitive.findAttribute("TANGENT");
                    it != primitive.attributes.end()) {
                    has_tangent = true;
                    const auto &accessor = gltf.accessors[it->accessorIndex];
                    size_t i = 0;
                    for (auto t: fastgltf::iterateAccessor<fastgltf::math::fvec4>(gltf, accessor)) {
                        vertices[i].tangent = {t.x(), t.y(), t.z(), t.w()};
                        ++i;
                    }
                }

                // Indices (generate sequential if non-indexed)
                std::vector<uint32_t> indices;
                if (primitive.indicesAccessor.has_value()) {
                    const auto &accessor = gltf.accessors[*primitive.indicesAccessor];
                    indices.reserve(accessor.count);
                    for (auto idx: fastgltf::iterateAccessor<std::uint32_t>(
                             gltf, accessor)) {
                        indices.push_back(idx);
                    }
                } else {
                    indices.resize(vertex_count);
                    for (size_t j = 0; j < vertex_count; ++j)
                        indices[j] = static_cast<uint32_t>(j);
                }

                // Generate tangents via MikkTSpace if missing (needs normal + uv0)
                if (!has_tangent && has_normals && has_uv0) {
                    framework::generate_tangents(vertices, indices);
                }

                // TEXCOORD_1 (optional, filled after MikkTSpace per design spec)
                if (const auto it = primitive.findAttribute("TEXCOORD_1");
                    it != primitive.attributes.end()) {
                    const auto &accessor = gltf.accessors[it->accessorIndex];
                    size_t i = 0;
                    for (auto uv: fastgltf::iterateAccessor<fastgltf::math::fvec2>(gltf, accessor)) {
                        vertices[i].uv1 = {uv.x(), uv.y()};
                        ++i;
                    }
                }

                // Create GPU vertex and index buffers
                const auto vb_size = vertices.size() * sizeof(framework::Vertex);
                const auto ib_size = indices.size() * sizeof(uint32_t);

                auto vb = resource_manager.create_buffer({
                    .size = vb_size,
                    .usage = rhi::BufferUsage::VertexBuffer | rhi::BufferUsage::TransferDst,
                    .memory = rhi::MemoryUsage::GpuOnly,
                });
                auto ib = resource_manager.create_buffer({
                    .size = ib_size,
                    .usage = rhi::BufferUsage::IndexBuffer | rhi::BufferUsage::TransferDst,
                    .memory = rhi::MemoryUsage::GpuOnly,
                });

                resource_manager.upload_buffer(vb, vertices.data(), vb_size);
                resource_manager.upload_buffer(ib, indices.data(), ib_size);

                meshes_.push_back({
                    .vertex_buffer = vb,
                    .index_buffer = ib,
                    .vertex_count = static_cast<uint32_t>(vertices.size()),
                    .index_count = static_cast<uint32_t>(indices.size()),
                });

                buffers_.push_back(vb);
                buffers_.push_back(ib);
            }
        }

        spdlog::info("Loaded {} mesh primitives", meshes_.size());
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
