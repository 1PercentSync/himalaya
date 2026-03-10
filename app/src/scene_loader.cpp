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

#include <cassert>
#include <filesystem>
#include <map>

namespace himalaya::app {
    namespace {
        // Decodes a glTF image from any source type into CPU RGBA8 pixel data.
        // Handles URI (fallback), Array, Vector, BufferView, and ByteView sources.
        framework::ImageData decode_gltf_image(const fastgltf::Asset &gltf,
                                               const fastgltf::Image &image,
                                               const std::filesystem::path &base_dir) {
            framework::ImageData result;

            std::visit(fastgltf::visitor{
                           [&](const fastgltf::sources::URI &) {
                               assert(false && "URI source should not appear with LoadExternalImages");
                           },
                           [&](const fastgltf::sources::Array &array) {
                               result = framework::load_image_from_memory(
                                   reinterpret_cast<const uint8_t *>(array.bytes.data()),
                                   array.bytes.size_bytes());
                           },
                           [&](const fastgltf::sources::Vector &vec) {
                               result = framework::load_image_from_memory(
                                   reinterpret_cast<const uint8_t *>(vec.bytes.data()),
                                   vec.bytes.size());
                           },
                           [&](const fastgltf::sources::BufferView &bv) {
                               const auto &view = gltf.bufferViews[bv.bufferViewIndex];
                               const auto &buffer = gltf.buffers[view.bufferIndex];
                               std::visit(fastgltf::visitor{
                                              [&](const fastgltf::sources::Array &arr) {
                                                  result = framework::load_image_from_memory(
                                                      reinterpret_cast<const uint8_t *>(arr.bytes.data()) + view.
                                                      byteOffset,
                                                      view.byteLength);
                                              },
                                              [&](const fastgltf::sources::Vector &v) {
                                                  result = framework::load_image_from_memory(
                                                      reinterpret_cast<const uint8_t *>(v.bytes.data()) + view.
                                                      byteOffset,
                                                      view.byteLength);
                                              },
                                              [&](const fastgltf::sources::ByteView &bytes) {
                                                  result = framework::load_image_from_memory(
                                                      reinterpret_cast<const uint8_t *>(bytes.bytes.data()) + view.
                                                      byteOffset,
                                                      view.byteLength);
                                              },
                                              [](auto &&) {
                                                  spdlog::error("Unsupported buffer data source for image");
                                                  std::abort();
                                              }
                                          }, buffer.data);
                           },
                           [&](const fastgltf::sources::ByteView &bytes) {
                               result = framework::load_image_from_memory(
                                   reinterpret_cast<const uint8_t *>(bytes.bytes.data()),
                                   bytes.bytes.size());
                           },
                           [](auto &&) {
                               spdlog::error("Unsupported image source type");
                               std::abort();
                           }
                       }, image.data);

            if (!result.valid()) {
                spdlog::error("Failed to decode glTF image '{}'",
                              std::string(image.name));
                std::abort();
            }

            return result;
        }

        // Converts a fastgltf sampler to our SamplerDesc.
        // Missing filter/wrap values use glTF defaults (linear filter, repeat wrap).
        rhi::SamplerDesc convert_gltf_sampler(const fastgltf::Sampler &sampler) {
            rhi::SamplerDesc desc{};

            // Mag filter (default: Linear)
            if (sampler.magFilter.has_value()) {
                desc.mag_filter = (*sampler.magFilter == fastgltf::Filter::Nearest)
                                      ? rhi::Filter::Nearest
                                      : rhi::Filter::Linear;
            } else {
                desc.mag_filter = rhi::Filter::Linear;
            }

            // Min filter encodes both minification and mip mode (default: Linear + Linear).
            // Nearest/Linear without MipMap suffix means no mipmapping — clamp max_lod to 0.
            if (sampler.minFilter.has_value()) {
                switch (*sampler.minFilter) {
                    case fastgltf::Filter::Nearest:
                        desc.min_filter = rhi::Filter::Nearest;
                        desc.mip_mode = rhi::SamplerMipMode::Nearest;
                        desc.max_lod = 0.0f;
                        break;
                    case fastgltf::Filter::Linear:
                        desc.min_filter = rhi::Filter::Linear;
                        desc.mip_mode = rhi::SamplerMipMode::Nearest;
                        desc.max_lod = 0.0f;
                        break;
                    case fastgltf::Filter::NearestMipMapNearest:
                        desc.min_filter = rhi::Filter::Nearest;
                        desc.mip_mode = rhi::SamplerMipMode::Nearest;
                        desc.max_lod = VK_LOD_CLAMP_NONE;
                        break;
                    case fastgltf::Filter::LinearMipMapNearest:
                        desc.min_filter = rhi::Filter::Linear;
                        desc.mip_mode = rhi::SamplerMipMode::Nearest;
                        desc.max_lod = VK_LOD_CLAMP_NONE;
                        break;
                    case fastgltf::Filter::NearestMipMapLinear:
                        desc.min_filter = rhi::Filter::Nearest;
                        desc.mip_mode = rhi::SamplerMipMode::Linear;
                        desc.max_lod = VK_LOD_CLAMP_NONE;
                        break;
                    case fastgltf::Filter::LinearMipMapLinear:
                        desc.min_filter = rhi::Filter::Linear;
                        desc.mip_mode = rhi::SamplerMipMode::Linear;
                        desc.max_lod = VK_LOD_CLAMP_NONE;
                        break;
                }
            } else {
                desc.min_filter = rhi::Filter::Linear;
                desc.mip_mode = rhi::SamplerMipMode::Linear;
                desc.max_lod = VK_LOD_CLAMP_NONE;
            }

            // Wrap modes
            auto convert_wrap = [](const fastgltf::Wrap wrap) -> rhi::SamplerWrapMode {
                switch (wrap) {
                    case fastgltf::Wrap::ClampToEdge: return rhi::SamplerWrapMode::ClampToEdge;
                    case fastgltf::Wrap::MirroredRepeat: return rhi::SamplerWrapMode::MirroredRepeat;
                    case fastgltf::Wrap::Repeat: return rhi::SamplerWrapMode::Repeat;
                }
                return rhi::SamplerWrapMode::Repeat;
            };

            desc.wrap_u = convert_wrap(sampler.wrapS);
            desc.wrap_v = convert_wrap(sampler.wrapT);
            desc.max_anisotropy = 0.0f;

            return desc;
        }

        // Converts fastgltf AlphaMode to our framework AlphaMode.
        framework::AlphaMode convert_alpha_mode(const fastgltf::AlphaMode mode) {
            switch (mode) {
                case fastgltf::AlphaMode::Opaque: return framework::AlphaMode::Opaque;
                case fastgltf::AlphaMode::Mask: return framework::AlphaMode::Mask;
                case fastgltf::AlphaMode::Blend: return framework::AlphaMode::Blend;
            }
            return framework::AlphaMode::Opaque;
        }
    }

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
        auto gltf_data = fastgltf::GltfDataBuffer::FromPath(path);
        if (gltf_data.error() != fastgltf::Error::None) {
            spdlog::error("Failed to read glTF file: {}", path);
            std::abort();
        }

        constexpr auto options = fastgltf::Options::LoadExternalBuffers
                                 | fastgltf::Options::LoadExternalImages;

        fastgltf::Parser parser;
        const auto base_dir = std::filesystem::path(path).parent_path();
        auto asset = parser.loadGltf(gltf_data.get(), base_dir, options);
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

        // --- Load samplers (one per glTF sampler, naturally deduplicated) ---
        for (const auto &s: gltf.samplers) {
            samplers_.push_back(resource_manager.create_sampler(convert_gltf_sampler(s)));
        }

        spdlog::info("Created {} samplers", samplers_.size());

        // --- Load materials ---
        // Texture cache: (gltf_texture_index, role) → bindless index.
        // Avoids creating duplicate GPU textures when multiple materials
        // reference the same glTF texture with the same role.
        std::map<std::pair<size_t, framework::TextureRole>, rhi::BindlessIndex> tex_cache;

        // Resolves a glTF texture reference to a bindless index, loading
        // the image on demand and caching the result.
        auto resolve_texture = [&](const size_t texture_index, const framework::TextureRole role) -> uint32_t {
            const auto key = std::make_pair(texture_index, role);
            if (const auto it = tex_cache.find(key); it != tex_cache.end()) {
                return it->second.index;
            }

            const auto &tex = gltf.textures[texture_index];
            assert(tex.imageIndex.has_value() && "glTF texture must have an image source");

            const auto sampler = tex.samplerIndex.has_value() ? samplers_[*tex.samplerIndex] : default_sampler;

            const auto pixels = decode_gltf_image(gltf, gltf.images[*tex.imageIndex], base_dir);
            const auto [image, bindless_index] = framework::create_texture(
                resource_manager, descriptor_manager, pixels, role, sampler);

            images_.push_back(image);
            bindless_indices_.push_back(bindless_index);
            tex_cache[key] = bindless_index;
            return bindless_index.index;
        };

        std::vector<framework::GPUMaterialData> gpu_materials;
        gpu_materials.reserve(gltf.materials.size());

        for (const auto &mat: gltf.materials) {
            framework::GPUMaterialData data{};
            // ReSharper disable once CppUseStructuredBinding
            const auto &pbr = mat.pbrData;

            // PBR metallic-roughness parameters
            data.base_color_factor = {
                pbr.baseColorFactor[0],
                pbr.baseColorFactor[1],
                pbr.baseColorFactor[2],
                pbr.baseColorFactor[3]
            };
            data.emissive_factor = {
                mat.emissiveFactor[0],
                mat.emissiveFactor[1],
                mat.emissiveFactor[2],
                0.0f
            };
            data.metallic_factor = pbr.metallicFactor;
            data.roughness_factor = pbr.roughnessFactor;
            data.normal_scale = mat.normalTexture.has_value() ? mat.normalTexture->scale : 1.0f;
            data.occlusion_strength = mat.occlusionTexture.has_value() ? mat.occlusionTexture->strength : 1.0f;
            data.alpha_cutoff = mat.alphaCutoff;
            data.alpha_mode = static_cast<uint32_t>(convert_alpha_mode(mat.alphaMode));
            data._padding = 0;

            // Texture references (UINT32_MAX = unset, filled by fill_material_defaults)
            data.base_color_tex = pbr.baseColorTexture.has_value()
                                      ? resolve_texture(pbr.baseColorTexture->textureIndex,
                                                        framework::TextureRole::Color)
                                      : UINT32_MAX;
            data.metallic_roughness_tex = pbr.metallicRoughnessTexture.has_value()
                                              ? resolve_texture(pbr.metallicRoughnessTexture->textureIndex,
                                                                framework::TextureRole::Linear)
                                              : UINT32_MAX;
            data.normal_tex = mat.normalTexture.has_value()
                                  ? resolve_texture(mat.normalTexture->textureIndex,
                                                    framework::TextureRole::Linear)
                                  : UINT32_MAX;
            data.occlusion_tex = mat.occlusionTexture.has_value()
                                     ? resolve_texture(mat.occlusionTexture->textureIndex,
                                                       framework::TextureRole::Linear)
                                     : UINT32_MAX;
            data.emissive_tex = mat.emissiveTexture.has_value()
                                    ? resolve_texture(mat.emissiveTexture->textureIndex,
                                                      framework::TextureRole::Color)
                                    : UINT32_MAX;

            // Fill unset texture slots with default textures
            framework::fill_material_defaults(data,
                                              default_textures.white.bindless_index,
                                              default_textures.flat_normal.bindless_index,
                                              default_textures.black.bindless_index);

            gpu_materials.push_back(data);

            material_instances_.push_back({
                .template_id = 0,
                .buffer_offset = static_cast<uint32_t>(material_instances_.size()),
                .alpha_mode = convert_alpha_mode(mat.alphaMode),
                .double_sided = mat.doubleSided,
            });
        }

        // Upload all material data to the GPU SSBO
        if (!gpu_materials.empty()) {
            material_system.upload_materials(gpu_materials);
        }

        spdlog::info("Loaded {} materials, {} GPU textures",
                     material_instances_.size(), images_.size());
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
