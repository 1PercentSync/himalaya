#pragma once

/**
 * @file scene_loader.h
 * @brief glTF scene loading: meshes, materials, textures, and scene graph.
 */

#include <himalaya/framework/material_system.h>
#include <himalaya/framework/mesh.h>
#include <himalaya/framework/scene_data.h>
#include <himalaya/framework/texture.h>
#include <himalaya/rhi/types.h>

#include <span>
#include <string>
#include <vector>

namespace fastgltf {
    struct Asset;
}

namespace himalaya::rhi {
    class DescriptorManager;
    class ResourceManager;
}

namespace himalaya::app {
    /**
     * @brief Loads glTF scenes and manages all loaded GPU resources.
     *
     * Parses a glTF file using fastgltf, creates GPU resources (vertex/index
     * buffers, textures, samplers), builds material instances, and populates
     * the scene instance list. Owns all loaded resource handles; call destroy()
     * to release them.
     *
     * Each glTF primitive is expanded into an independent MeshInstance.
     * Samplers are naturally deduplicated by glTF sampler index.
     *
     * Supports scene switching: destroy() then load() replaces the scene.
     */
    class SceneLoader {
    public:
        /**
         * @brief Loads a glTF scene from the given file path.
         *
         * Must be called within a Context::begin_immediate() / end_immediate()
         * scope. On failure, logs the error, cleans up any partial state, and
         * returns false (caller gets an empty scene).
         *
         * @param path               Path to the .gltf or .glb file.
         * @param resource_manager   Resource manager for buffer/image/sampler creation.
         * @param descriptor_manager Descriptor manager for bindless texture registration.
         * @param material_system    Material system for SSBO upload.
         * @param default_textures   Default textures for missing material slots.
         * @param default_sampler    Fallback sampler for textures without a glTF sampler.
         * @param rt_supported       When true, vertex/index buffers get ShaderDeviceAddress usage.
         * @return true on success, false on failure (scene remains empty).
         */
        bool load(const std::string &path,
                  rhi::ResourceManager &resource_manager,
                  rhi::DescriptorManager &descriptor_manager,
                  framework::MaterialSystem &material_system,
                  const framework::DefaultTextures &default_textures,
                  rhi::SamplerHandle default_sampler,
                  bool rt_supported);

        /**
         * @brief Destroys all loaded resources (buffers, images, samplers, bindless entries).
         *
         * Safe to call even if load() was never called.
         * Does not destroy the MaterialSystem's SSBO (MaterialSystem owns that).
         */
        void destroy();

        /** @brief Returns the loaded meshes (vertex/index buffer handles and counts). */
        [[nodiscard]] std::span<const framework::Mesh> meshes() const;

        /** @brief Returns the loaded material instances. */
        [[nodiscard]] std::span<const framework::MaterialInstance> material_instances() const;

        /** @brief Returns all scene mesh instances (one per node-primitive combination). */
        [[nodiscard]] std::span<const framework::MeshInstance> mesh_instances() const;

        /** @brief Returns the loaded directional lights (may be empty). */
        [[nodiscard]] std::span<const framework::DirectionalLight> directional_lights() const;

        /** @brief Returns per-mesh CPU vertex data (parallel to meshes()). Available until destroy(). */
        [[nodiscard]] std::span<const std::vector<framework::Vertex>> cpu_vertices() const;

        /** @brief Returns per-mesh CPU index data (parallel to meshes()). Available until destroy(). */
        [[nodiscard]] std::span<const std::vector<uint32_t>> cpu_indices() const;

        /** @brief Returns GPU material data array. Available until destroy(). */
        [[nodiscard]] std::span<const framework::GPUMaterialData> gpu_materials() const;

        /** @brief Returns the number of GPU texture images loaded from the scene. */
        [[nodiscard]] uint32_t texture_count() const;

        /**
         * @brief Scene AABB (union of all mesh instance world_bounds).
         *
         * Available after load() completes. Empty scene returns a degenerate
         * AABB (min = max = 0). Used by Application to initialize
         * ShadowConfig.max_distance and camera position.
         */
        [[nodiscard]] const framework::AABB &scene_bounds() const;

        /**
         * @brief Content hash of the scene file (XXH3_128, hex).
         *
         * Computed at load time from the glTF/GLB file bytes. Empty if
         * no scene loaded. Used as part of bake cache keys.
         */
        [[nodiscard]] const std::string &scene_hash() const;

        /**
         * @brief Composite hash of all scene texture source bytes.
         *
         * Computed at load time by concatenating per-texture source hashes
         * (in unique_entries order) and hashing the result. Used as part
         * of the bake cache key to invalidate when any texture changes.
         * Empty string if no textures were loaded.
         */
        [[nodiscard]] const std::string &scene_textures_hash() const;

    private:
        // ---- Subsystem references (set during load) ----

        /** @brief Resource manager for resource destruction in destroy(). */
        rhi::ResourceManager *resource_manager_ = nullptr;

        /** @brief Descriptor manager for bindless unregistration in destroy(). */
        rhi::DescriptorManager *descriptor_manager_ = nullptr;

        /** @brief Whether RT is supported (vertex/index buffers need ShaderDeviceAddress). */
        bool rt_supported_ = false;

        // ---- Loaded scene data ----

        /** @brief GPU mesh resources (one per glTF primitive). */
        std::vector<framework::Mesh> meshes_;

        /** @brief Material instances (one per glTF material). */
        std::vector<framework::MaterialInstance> material_instances_;

        /** @brief Scene mesh instances (one per node-primitive combination). */
        std::vector<framework::MeshInstance> mesh_instances_;

        /** @brief Directional lights extracted from glTF KHR_lights_punctual. */
        std::vector<framework::DirectionalLight> directional_lights_;

        /** @brief CPU vertex data per mesh (parallel to meshes_). Retained for EmissiveLightBuilder. */
        std::vector<std::vector<framework::Vertex>> cpu_vertices_;

        /** @brief CPU index data per mesh (parallel to meshes_). Retained for EmissiveLightBuilder. */
        std::vector<std::vector<uint32_t>> cpu_indices_;

        /** @brief GPU material data array. Retained for EmissiveLightBuilder emissive_factor lookup. */
        std::vector<framework::GPUMaterialData> gpu_materials_;

        /** @brief Union AABB of all mesh instance world_bounds, computed at load time. */
        framework::AABB scene_bounds_{};

        /** @brief Content hash of the scene file (XXH3_128, hex). */
        std::string scene_hash_;

        /** @brief Composite hash of all scene texture source bytes (XXH3_128, hex). */
        std::string scene_textures_hash_;

        // ---- Resource handles for cleanup ----

        /** @brief Vertex and index buffer handles. */
        std::vector<rhi::BufferHandle> buffers_;

        /** @brief Texture image handles. */
        std::vector<rhi::ImageHandle> images_;

        /** @brief Bindless indices for registered textures. */
        std::vector<rhi::BindlessIndex> bindless_indices_;

        /** @brief Sampler handles created from glTF sampler definitions. */
        std::vector<rhi::SamplerHandle> samplers_;

        // ---- Private loading stages ----

        /** @brief Intermediate data from mesh loading, consumed by build_mesh_instances(). */
        struct MeshLoadResult {
            /** @brief glTF mesh index → starting index in meshes_. Last entry = sentinel. */
            std::vector<uint32_t> prim_offsets;

            /** @brief Per-primitive material index (parallel to meshes_). */
            std::vector<uint32_t> material_ids;

            /** @brief Per-primitive local-space AABB (parallel to meshes_). */
            std::vector<framework::AABB> local_bounds;
        };

        /** @brief Loads all mesh primitives: vertex/index buffers, local AABBs, material IDs. */
        MeshLoadResult load_meshes(const fastgltf::Asset &gltf);

        /** @brief Loads samplers, textures, and materials from the glTF asset. */
        void load_materials(const fastgltf::Asset &gltf,
                            const std::string &base_dir,
                            framework::MaterialSystem &material_system,
                            const framework::DefaultTextures &default_textures,
                            rhi::SamplerHandle default_sampler);

        /** @brief Traverses the scene graph and creates MeshInstances with world transforms. */
        void build_mesh_instances(fastgltf::Asset &gltf,
                                  const MeshLoadResult &mesh_data);
    };
} // namespace himalaya::app
