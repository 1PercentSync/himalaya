#pragma once

/**
 * @file bake_data_manager.h
 * @brief Bake data lifecycle management: scan, validate, load, unload.
 */

#include <himalaya/rhi/types.h>

#include <cstdint>
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
     * completeness, and provides the available angle list to the UI.
     * Loading/unloading (KTX2 -> GPU -> bindless) added in Step 6.
     *
     * Owned by Renderer. Application drives scan() at appropriate times
     * (scene/HDR load, bake complete, cache clear).
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
         * plus sampler handles needed by load_angle() (Step 6).
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

    private:
        /** @brief GPU resource pool (used by load/unload in Step 6). */
        rhi::ResourceManager* resource_manager_ = nullptr;

        /** @brief Descriptor set management (used by load/unload in Step 6). */
        rhi::DescriptorManager* descriptor_manager_ = nullptr;

        /** @brief Linear clamp sampler for lightmap textures (Step 6). */
        rhi::SamplerHandle lightmap_sampler_{};

        /** @brief Default linear repeat sampler for probe cubemaps (Step 6). */
        rhi::SamplerHandle probe_sampler_{};

        /** @brief Validated available angles (populated by scan()). */
        std::vector<AngleInfo> available_angles_;
    };

} // namespace himalaya::framework
