#pragma once

/**
 * @file bake_data_manager.h
 * @brief Bake data lifecycle management: scan, validate, load, unload.
 */

#include <himalaya/framework/lightmap_uv.h>
#include <himalaya/rhi/types.h>

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace himalaya::rhi {
    class ResourceManager;
    class DescriptorManager;
} // namespace himalaya::rhi

namespace himalaya::framework {

    /**
     * @brief Manages baked lighting data lifecycle.
     *
     * Scans the bake cache directory for completed angles, validates file
     * completeness, loads angle data (KTX2 -> GPU -> bindless), and
     * provides per-instance lightmap/probe indices for InstanceBuffer filling.
     *
     * Owned by Renderer. Application drives scan() at appropriate times
     * (scene/HDR load, bake complete, cache clear). Renderer drives
     * load_angle() / unload_angle() within immediate command scopes.
     */
    class BakeDataManager {
    public:
        /**
         * @brief Summary of a single validated bake angle in the cache.
         */
        struct AngleInfo {
            uint32_t rotation;       ///< Rotation angle in integer degrees (0-359).
            uint32_t lightmap_count; ///< Number of lightmap KTX2 files at this angle.
            uint32_t probe_count;    ///< Number of probe KTX2 files at this angle.
        };

        /**
         * @brief Initializes subsystem references for later use.
         *
         * Stores non-owning references to ResourceManager and DescriptorManager,
         * plus sampler handles needed by load_angle().
         *
         * @param rm               GPU resource pool.
         * @param dm               Descriptor set management.
         * @param lightmap_sampler Linear clamp sampler for lightmap textures.
         * @param probe_sampler    Default linear repeat sampler for probe cubemaps.
         */
        void init(rhi::ResourceManager& rm, rhi::DescriptorManager& dm,
                  rhi::SamplerHandle lightmap_sampler,
                  rhi::SamplerHandle probe_sampler);

        /**
         * @brief Releases internal resources and clears state.
         *
         * Calls unload_angle() if data is currently loaded.
         */
        void destroy();

        /**
         * @brief Scans cache_root()/bake/ for completed bake angles.
         *
         * For each angle found via manifest filenames, validates that all
         * expected lightmap KTX2 files and probe KTX2 files exist. Only
         * fully complete angles are added to the available list.
         *
         * @param lightmap_keys Per-bakeable-instance lightmap cache key hashes.
         * @param probe_set_key Probe set cache key hash (scene + hdr + scene_textures).
         */
        void scan(std::span<const std::string> lightmap_keys,
                  const std::string& probe_set_key);

        /**
         * @brief Reads per-instance UV data for a bake angle from disk cache.
         *
         * Returns a vector parallel to lightmap_keys. Each entry is the
         * LightmapUVResult loaded from the corresponding UV bin file, or
         * nullopt on read failure. Caller uses these to rebuild VB/IB
         * before calling load_angle().
         *
         * @param rotation_int  Bake angle in integer degrees (0-359).
         * @param lightmap_keys Per-bakeable-instance lightmap cache key hashes.
         */
        [[nodiscard]] static std::vector<std::optional<LightmapUVResult>>
        read_angle_uv_data(uint32_t rotation_int,
                           std::span<const std::string> lightmap_keys);

        /**
         * @brief Loads a bake angle: reads KTX2 files, creates GPU images,
         *        registers bindless textures, and computes per-instance indices.
         *
         * Must be called within an active immediate command scope
         * (Context::begin_immediate / end_immediate). Caller must ensure
         * GPU is idle before calling (vkQueueWaitIdle). If data is already
         * loaded, unloads the previous angle first.
         *
         * @param rotation_int        Bake angle in integer degrees (0-359).
         * @param lightmap_keys       Per-bakeable-instance lightmap cache key hashes.
         * @param bakeable_indices    Mapping from bakeable index to full instance index
         *                            (parallel to lightmap_keys).
         * @param probe_set_key       Probe set cache key hash.
         * @param total_instance_count Total number of mesh instances (for lightmap
         *                            index array sizing).
         */
        void load_angle(uint32_t rotation_int,
                        std::span<const std::string> lightmap_keys,
                        std::span<const uint32_t> bakeable_indices,
                        const std::string& probe_set_key,
                        uint32_t total_instance_count);

        /**
         * @brief Unloads the current angle: unregisters bindless descriptors,
         *        destroys GPU images and probe buffer, clears per-instance indices.
         *
         * Caller must ensure GPU is idle before calling (vkQueueWaitIdle).
         * Safe to call when nothing is loaded (no-op).
         */
        void unload_angle();

        /**
         * @brief Returns the list of validated available bake angles.
         *
         * Populated by scan(). Empty before first scan or when no valid
         * angles exist.
         */
        [[nodiscard]] std::span<const AngleInfo> available_angles() const;

        /**
         * @brief Returns true if at least one validated bake angle is available.
         */
        [[nodiscard]] bool has_bake_data() const;

        /**
         * @brief Returns true if a bake angle is currently loaded on the GPU.
         */
        [[nodiscard]] bool is_loaded() const;

        /**
         * @brief Returns the currently loaded angle in integer degrees.
         *
         * Only meaningful when is_loaded() returns true.
         */
        [[nodiscard]] uint32_t loaded_rotation() const;

        /**
         * @brief Returns per-instance lightmap bindless indices.
         *
         * Parallel to mesh_instances (UINT32_MAX = no lightmap for that instance).
         * Empty when no angle is loaded.
         */
        [[nodiscard]] std::span<const uint32_t> lightmap_indices() const;

        /**
         * @brief Returns the number of probes loaded in the current angle.
         *
         * Returns 0 when no angle is loaded.
         */
        [[nodiscard]] uint32_t loaded_probe_count() const;

        /**
         * @brief Returns the probe grid spacing read from the manifest.
         *
         * Returns 0 when no angle is loaded. Used by Step 3 to construct
         * the 3D spatial grid.
         */
        [[nodiscard]] float probe_spacing() const;

    private:
        /** @brief GPU resource pool. */
        rhi::ResourceManager* resource_manager_ = nullptr;

        /** @brief Descriptor set management. */
        rhi::DescriptorManager* descriptor_manager_ = nullptr;

        /** @brief Linear clamp sampler for lightmap textures. */
        rhi::SamplerHandle lightmap_sampler_{};

        /** @brief Default linear repeat sampler for probe cubemaps. */
        rhi::SamplerHandle probe_sampler_{};

        /** @brief Validated available angles (populated by scan()). */
        std::vector<AngleInfo> available_angles_;

        // --- Loaded angle state ---

        /** @brief True when a bake angle is loaded on the GPU. */
        bool is_loaded_ = false;

        /** @brief Currently loaded rotation angle in integer degrees. */
        uint32_t loaded_rotation_ = 0;

        /** @brief GPU images for loaded lightmaps (parallel to lightmap_bindless_). */
        std::vector<rhi::ImageHandle> lightmap_images_;

        /** @brief Bindless indices for loaded lightmaps in textures[] (parallel to lightmap_images_). */
        std::vector<rhi::BindlessIndex> lightmap_bindless_;

        /** @brief GPU cubemap images for loaded probes (parallel to probe_bindless_). */
        std::vector<rhi::ImageHandle> probe_images_;

        /** @brief Bindless indices for loaded probe cubemaps in cubemaps[] (parallel to probe_images_). */
        std::vector<rhi::BindlessIndex> probe_bindless_;

        /** @brief ProbeBuffer SSBO (Set 0, Binding 9). Invalid when not loaded. */
        rhi::BufferHandle probe_buffer_{};

        /** @brief Per-instance lightmap bindless indices (parallel to mesh_instances, UINT32_MAX = none). */
        std::vector<uint32_t> lightmap_indices_;

        /** @brief Number of probes loaded in the current angle (0 when not loaded). */
        uint32_t loaded_probe_count_ = 0;

        /** @brief Probe grid spacing from manifest (0 when not loaded). */
        float probe_spacing_ = 0.0f;
    };

} // namespace himalaya::framework
