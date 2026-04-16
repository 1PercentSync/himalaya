/**
 * @file bake_data_manager.cpp
 * @brief BakeDataManager implementation: bake cache scanning and validation.
 */

#include <himalaya/framework/bake_data_manager.h>
#include <himalaya/framework/cache.h>

#include <spdlog/spdlog.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <set>

namespace himalaya::framework {

    void BakeDataManager::init(rhi::ResourceManager& rm, rhi::DescriptorManager& dm,
                               const rhi::SamplerHandle lightmap_sampler,
                               const rhi::SamplerHandle probe_sampler) {
        resource_manager_ = &rm;
        descriptor_manager_ = &dm;
        lightmap_sampler_ = lightmap_sampler;
        probe_sampler_ = probe_sampler;
    }

    void BakeDataManager::destroy() {
        available_angles_.clear();
        resource_manager_ = nullptr;
        descriptor_manager_ = nullptr;
    }

    void BakeDataManager::scan(const std::span<const std::string> lightmap_keys,
                               const std::string& probe_set_key) {
        available_angles_.clear();

        if (probe_set_key.empty()) { return; }

        const auto bake_dir = cache_root() / "bake";
        std::error_code ec;
        if (!std::filesystem::exists(bake_dir, ec)) { return; }

        // Collect unique rotation angles from manifest filenames.
        // Manifest naming: <probe_set_key>_rot<NNN>_manifest.bin
        const std::string prefix = probe_set_key + "_rot";
        std::set<uint32_t> found_angles;

        for (const auto& entry : std::filesystem::directory_iterator(bake_dir, ec)) {
            if (!entry.is_regular_file()) { continue; }
            const auto stem = entry.path().stem().string();
            if (stem.starts_with(prefix) && stem.ends_with("_manifest")) {
                // Extract rotation: <key>_rot<NNN>_manifest
                const auto rot_start = prefix.size();
                const auto rot_end = stem.find('_', rot_start);
                if (rot_end != std::string::npos) {
                    try {
                        const auto rot = std::stoul(
                            stem.substr(rot_start, rot_end - rot_start));
                        found_angles.insert(static_cast<uint32_t>(rot));
                    } catch (...) {}
                }
            }
        }

        // For each angle, verify completeness before adding to list.
        const auto expected_lm = static_cast<uint32_t>(lightmap_keys.size());

        for (const uint32_t rot : found_angles) {
            char rot_str[4];
            std::snprintf(rot_str, sizeof(rot_str), "%03u", rot);
            const std::string rot_suffix = "_rot" + std::string(rot_str);

            // Lightmaps: verify every per-instance key file exists.
            uint32_t lm_count = 0;
            for (const auto& key : lightmap_keys) {
                const auto path = cache_path("bake", key + rot_suffix, ".ktx2");
                if (std::filesystem::exists(path, ec)) {
                    ++lm_count;
                }
            }
            if (lm_count < expected_lm) { continue; }

            // Read probe count from manifest header (uint32_t at offset 0).
            uint32_t manifest_probe_count = 0;
            {
                const auto manifest_path = cache_path(
                    "bake", probe_set_key + rot_suffix + "_manifest", ".bin");
                std::ifstream mf(manifest_path, std::ios::binary);
                if (mf) {
                    mf.read(reinterpret_cast<char*>(&manifest_probe_count),
                            sizeof(uint32_t));
                }
            }

            // Probes: verify every probe KTX2 exists.
            uint32_t probe_count = 0;
            for (uint32_t pi = 0; pi < manifest_probe_count; ++pi) {
                char probe_suffix[16];
                std::snprintf(probe_suffix, sizeof(probe_suffix), "_probe%03u", pi);
                const auto path = cache_path(
                    "bake",
                    probe_set_key + rot_suffix + std::string(probe_suffix),
                    ".ktx2");
                if (std::filesystem::exists(path, ec)) {
                    ++probe_count;
                }
            }
            if (probe_count < manifest_probe_count) { continue; }

            available_angles_.push_back({rot, lm_count, probe_count});
        }

        spdlog::info("BakeDataManager: scan found {} valid angle(s)",
                     available_angles_.size());
    }

    std::span<const BakeDataManager::AngleInfo>
    BakeDataManager::available_angles() const {
        return available_angles_;
    }

    bool BakeDataManager::has_bake_data() const {
        return !available_angles_.empty();
    }

} // namespace himalaya::framework
