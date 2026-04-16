/**
 * @file bake_data_manager.cpp
 * @brief BakeDataManager implementation: bake cache scanning, validation, loading, unloading.
 */

#include <himalaya/framework/bake_data_manager.h>
#include <himalaya/framework/cache.h>
#include <himalaya/framework/ktx2.h>
#include <himalaya/framework/scene_data.h>
#include <himalaya/rhi/descriptors.h>
#include <himalaya/rhi/resources.h>

#include <spdlog/spdlog.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <set>

namespace himalaya::framework {

    namespace {
        /**
         * Creates a GPU image from KTX2 data and records upload commands.
         * Handles both 2D textures (face_count=1) and cubemaps (face_count=6).
         * Must be called within an active immediate scope.
         */
        void upload_ktx2_to_gpu(rhi::ResourceManager& rm,
                                const Ktx2Data& ktx2,
                                rhi::ImageHandle& out_handle,
                                const char* debug_name) {
            const rhi::ImageDesc desc{
                .width = ktx2.base_width,
                .height = ktx2.base_height,
                .depth = 1,
                .mip_levels = ktx2.level_count,
                .array_layers = ktx2.face_count,
                .sample_count = 1,
                .format = ktx2.format,
                .usage = rhi::ImageUsage::Sampled | rhi::ImageUsage::TransferDst,
            };
            out_handle = rm.create_image(desc, debug_name);

            std::vector<rhi::ResourceManager::MipUploadRegion> regions(ktx2.level_count);
            for (uint32_t i = 0; i < ktx2.level_count; ++i) {
                regions[i] = {
                    .buffer_offset = ktx2.levels[i].offset,
                    .width = std::max(1u, ktx2.base_width >> i),
                    .height = std::max(1u, ktx2.base_height >> i),
                };
            }

            rm.upload_image_all_levels(out_handle, ktx2.blob.data(), ktx2.blob.size(),
                                       regions, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT);
        }
    } // anonymous namespace

    void BakeDataManager::init(rhi::ResourceManager& rm, rhi::DescriptorManager& dm,
                               const rhi::SamplerHandle lightmap_sampler,
                               const rhi::SamplerHandle probe_sampler) {
        resource_manager_ = &rm;
        descriptor_manager_ = &dm;
        lightmap_sampler_ = lightmap_sampler;
        probe_sampler_ = probe_sampler;
    }

    void BakeDataManager::destroy() {
        unload_angle();
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

    // ---- Loading / Unloading ----

    void BakeDataManager::load_angle(const uint32_t rotation_int,
                                     const std::span<const std::string> lightmap_keys,
                                     const std::span<const uint32_t> bakeable_indices,
                                     const std::string& probe_set_key,
                                     const std::span<const MeshInstance> mesh_instances) {
        // Clean up any previously loaded data
        unload_angle();

        const auto total_instances = static_cast<uint32_t>(mesh_instances.size());

        // Initialize per-instance indices with sentinel values
        lightmap_indices_.assign(total_instances, UINT32_MAX);
        probe_indices_.assign(total_instances, UINT32_MAX);

        // Build rotation suffix string (e.g. "_rot045")
        char rot_str[4];
        std::snprintf(rot_str, sizeof(rot_str), "%03u", rotation_int);
        const std::string rot_suffix = "_rot" + std::string(rot_str);

        // --- Lightmap loading ---
        uint32_t loaded_lm = 0;
        for (size_t i = 0; i < lightmap_keys.size(); ++i) {
            const auto path = cache_path("bake", lightmap_keys[i] + rot_suffix, ".ktx2");
            auto ktx2 = read_ktx2(path);
            if (!ktx2) {
                spdlog::warn("BakeDataManager: failed to read lightmap KTX2: {}",
                             path.string());
                continue;
            }

            rhi::ImageHandle image;
            const std::string name = "Lightmap [" + std::to_string(i) + "]";
            upload_ktx2_to_gpu(*resource_manager_, *ktx2, image, name.c_str());

            const auto bindless = descriptor_manager_->register_texture(
                image, lightmap_sampler_);
            lightmap_images_.push_back(image);
            lightmap_bindless_.push_back(bindless);

            // Map bakeable index to full instance index
            if (i < bakeable_indices.size()) {
                lightmap_indices_[bakeable_indices[i]] = bindless.index;
            }
            ++loaded_lm;
        }

        // --- Probe loading ---
        uint32_t manifest_probe_count = 0;
        std::vector<glm::vec3> probe_positions;

        // Read manifest file (binary: uint32_t count + vec3[count] positions)
        {
            const auto manifest_path = cache_path(
                "bake", probe_set_key + rot_suffix + "_manifest", ".bin");
            std::ifstream mf(manifest_path, std::ios::binary);
            if (mf) {
                mf.read(reinterpret_cast<char*>(&manifest_probe_count),
                        sizeof(uint32_t));
                if (manifest_probe_count > 0) {
                    probe_positions.resize(manifest_probe_count);
                    mf.read(reinterpret_cast<char*>(probe_positions.data()),
                            static_cast<std::streamsize>(
                                manifest_probe_count * sizeof(glm::vec3)));
                }
            }
        }

        // Load probe cubemaps and build GPUProbeData array
        std::vector<GPUProbeData> gpu_probes;
        gpu_probes.reserve(manifest_probe_count);

        for (uint32_t pi = 0; pi < manifest_probe_count; ++pi) {
            char probe_suffix[16];
            std::snprintf(probe_suffix, sizeof(probe_suffix), "_probe%03u", pi);
            const auto path = cache_path(
                "bake",
                probe_set_key + rot_suffix + std::string(probe_suffix),
                ".ktx2");
            auto ktx2 = read_ktx2(path);
            if (!ktx2) {
                spdlog::warn("BakeDataManager: failed to read probe KTX2: {}",
                             path.string());
                continue;
            }

            rhi::ImageHandle image;
            const std::string name = "Probe Cubemap [" + std::to_string(pi) + "]";
            upload_ktx2_to_gpu(*resource_manager_, *ktx2, image, name.c_str());

            const auto bindless = descriptor_manager_->register_cubemap(
                image, probe_sampler_);
            probe_images_.push_back(image);
            probe_bindless_.push_back(bindless);

            GPUProbeData pd{};
            pd.position = probe_positions[pi];
            pd.cubemap_index = bindless.index;
            gpu_probes.push_back(pd);
        }

        // Create and upload ProbeBuffer SSBO (Set 0, Binding 9)
        const auto loaded_probes = static_cast<uint32_t>(gpu_probes.size());
        if (loaded_probes > 0) {
            const auto buf_size = static_cast<uint64_t>(loaded_probes)
                                  * sizeof(GPUProbeData);
            probe_buffer_ = resource_manager_->create_buffer({
                .size = buf_size,
                .usage = rhi::BufferUsage::StorageBuffer
                         | rhi::BufferUsage::TransferDst,
                .memory = rhi::MemoryUsage::GpuOnly,
            }, "ProbeBuffer SSBO");
            resource_manager_->upload_buffer(probe_buffer_,
                                             gpu_probes.data(), buf_size);
            descriptor_manager_->write_set0_probe_buffer(probe_buffer_, buf_size);
        }

        // --- CPU probe-to-instance assignment (nearest by AABB center) ---
        if (loaded_probes > 0) {
            for (size_t i = 0; i < mesh_instances.size(); ++i) {
                const auto& bounds = mesh_instances[i].world_bounds;
                const auto center = (bounds.min + bounds.max) * 0.5f;

                float min_dist_sq = std::numeric_limits<float>::max();
                uint32_t nearest = UINT32_MAX;
                for (uint32_t pi = 0; pi < loaded_probes; ++pi) {
                    const auto diff = center - gpu_probes[pi].position;
                    const float dist_sq = glm::dot(diff, diff);
                    if (dist_sq < min_dist_sq) {
                        min_dist_sq = dist_sq;
                        nearest = pi;
                    }
                }
                probe_indices_[i] = nearest;
            }
        }

        is_loaded_ = true;
        loaded_rotation_ = rotation_int;

        spdlog::info("BakeDataManager: loaded angle {} — {} lightmaps, {} probes",
                     rotation_int, loaded_lm, loaded_probes);
    }

    void BakeDataManager::unload_angle() {
        if (!is_loaded_) { return; }

        // Unregister and destroy lightmap textures
        for (size_t i = 0; i < lightmap_bindless_.size(); ++i) {
            descriptor_manager_->unregister_texture(lightmap_bindless_[i]);
            resource_manager_->destroy_image(lightmap_images_[i]);
        }
        lightmap_images_.clear();
        lightmap_bindless_.clear();

        // Unregister and destroy probe cubemaps
        for (size_t i = 0; i < probe_bindless_.size(); ++i) {
            descriptor_manager_->unregister_cubemap(probe_bindless_[i]);
            resource_manager_->destroy_image(probe_images_[i]);
        }
        probe_images_.clear();
        probe_bindless_.clear();

        // Destroy ProbeBuffer SSBO
        if (probe_buffer_.valid()) {
            resource_manager_->destroy_buffer(probe_buffer_);
            probe_buffer_ = {};
        }

        // Clear per-instance indices
        lightmap_indices_.clear();
        probe_indices_.clear();

        is_loaded_ = false;
        loaded_rotation_ = 0;

        spdlog::info("BakeDataManager: unloaded angle data");
    }

    // ---- Accessors ----

    bool BakeDataManager::is_loaded() const {
        return is_loaded_;
    }

    uint32_t BakeDataManager::loaded_rotation() const {
        return loaded_rotation_;
    }

    std::span<const uint32_t> BakeDataManager::lightmap_indices() const {
        return lightmap_indices_;
    }

    std::span<const uint32_t> BakeDataManager::probe_indices() const {
        return probe_indices_;
    }

} // namespace himalaya::framework
