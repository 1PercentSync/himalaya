#include <himalaya/framework/cached_shader_compiler.h>
#include <himalaya/framework/cache.h>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <filesystem>
#include <fstream>

namespace himalaya::framework {
    // Returns the single-char stage prefix matching rhi::ShaderCompiler's
    // make_cache_key(), so the disk key is derived from the same composite string.
    static char stage_prefix(const rhi::ShaderStage stage) {
        switch (stage) {
            case rhi::ShaderStage::Vertex: return 'V';
            case rhi::ShaderStage::Fragment: return 'F';
            case rhi::ShaderStage::Compute: return 'C';
            case rhi::ShaderStage::RayGen: return 'R';
            case rhi::ShaderStage::ClosestHit: return 'H';
            case rhi::ShaderStage::AnyHit: return 'A';
            case rhi::ShaderStage::Miss: return 'M';
        }
        std::abort();
    }

    // Computes a 32-char hex disk key: XXH3_128(stage_prefix + path + '\0' + source_content).
    // Including the path prevents collisions between different files with identical source
    // (their includes resolve differently based on relative directory).
    static std::string compute_disk_key(const std::string &source,
                                        const rhi::ShaderStage stage,
                                        const std::string &path) {
        std::string composite;
        composite.reserve(1 + path.size() + 1 + source.size());
        composite += stage_prefix(stage);
        composite += path;
        composite += '\0';
        composite += source;
        return content_hash(composite.data(), composite.size());
    }

    // Reads a file in text mode and hashes the content.
    // Text mode matches how shaderc's FileIncluder reads includes (\r\n → \n on Windows),
    // so the hash is consistent with the compile-time captured content.
    static std::string text_mode_content_hash(const std::filesystem::path &file) {
        std::ifstream ifs(file);
        if (!ifs) {
            return {};
        }
        const std::string content{std::istreambuf_iterator<char>(ifs),
                                  std::istreambuf_iterator<char>()};
        return content_hash(content.data(), content.size());
    }

    // Validates .meta include hashes against current files on disk.
    // Returns true if all includes match, false on any mismatch or read failure.
    static bool validate_meta_includes(const nlohmann::json &meta, const std::filesystem::path &include_root) {
        if (!meta.contains("includes") || !meta["includes"].is_array()) {
            return true; // No includes to validate
        }

        return std::ranges::all_of(meta["includes"], [&](const auto &entry) {
            if (!entry.contains("path") || !entry.contains("hash")) {
                return false;
            }
            const auto rel_path = entry["path"].template get<std::string>();
            const auto expected_hash = entry["hash"].template get<std::string>();
            const auto current_hash = text_mode_content_hash(include_root / rel_path);
            return !current_hash.empty() && current_hash == expected_hash;
        });
    }

    // Attempts to load cached SPIR-V from disk.
    // Returns the SPIR-V bytecode on hit, or empty vector on miss.
    static std::vector<uint32_t> try_load_disk_cache(
        const std::string &category,
        const std::string &disk_key,
        const std::filesystem::path &include_root) {
        const auto spv_path = cache_path(category, disk_key, ".spv");
        const auto meta_path = cache_path(category, disk_key, ".meta");

        // Both files must exist
        if (!std::filesystem::exists(spv_path) || !std::filesystem::exists(meta_path)) {
            return {};
        }

        // Read and validate .meta
        std::ifstream meta_file(meta_path);
        if (!meta_file.is_open()) {
            return {};
        }

        nlohmann::json meta;
        try {
            meta_file >> meta;
        } catch (const nlohmann::json::parse_error &) {
            return {};
        }

        if (!validate_meta_includes(meta, include_root)) {
            return {};
        }

        // Read .spv binary
        std::ifstream spv_file(spv_path, std::ios::binary | std::ios::ate);
        if (!spv_file.is_open()) {
            return {};
        }

        const auto file_size = static_cast<size_t>(spv_file.tellg());
        if (file_size == 0 || file_size % sizeof(uint32_t) != 0) {
            return {};
        }

        spv_file.seekg(0);
        std::vector<uint32_t> spirv(file_size / sizeof(uint32_t));
        spv_file.read(reinterpret_cast<char *>(spirv.data()), static_cast<std::streamsize>(file_size));
        if (!spv_file) {
            return {};
        }
        return spirv;
    }

    // Writes compiled SPIR-V and include metadata to disk.
    // Hashes are computed from the in-memory content captured at compile time,
    // not re-read from disk, to avoid TOCTOU inconsistency.
    static void write_disk_cache(
        const std::string &category,
        const std::string &disk_key,
        const std::vector<uint32_t> &spirv,
        const std::vector<std::pair<std::string, std::string>> &included_files) {
        // Write .spv binary
        const auto spv_path = cache_path(category, disk_key, ".spv");
        std::ofstream spv_file(spv_path, std::ios::binary);
        if (!spv_file.is_open()) {
            spdlog::warn("Shader disk cache: failed to write {}", spv_path.string());
            return;
        }
        spv_file.write(reinterpret_cast<const char *>(spirv.data()),
                       static_cast<std::streamsize>(spirv.size() * sizeof(uint32_t)));

        // Build .meta JSON — hash from compile-time captured content
        nlohmann::json meta;
        auto &includes = meta["includes"];
        includes = nlohmann::json::array();

        for (const auto &[path, file_content] : included_files) {
            const auto hash = content_hash(file_content.data(), file_content.size());
            includes.push_back({{"path", path}, {"hash", hash}});
        }

        // Write .meta JSON
        const auto meta_path = cache_path(category, disk_key, ".meta");
        std::ofstream meta_file(meta_path);
        if (!meta_file.is_open()) {
            spdlog::warn("Shader disk cache: failed to write {}", meta_path.string());
            return;
        }
        meta_file << meta.dump(2);
    }

    void CachedShaderCompiler::set_cache_category(const std::string &category) {
        category_ = category;
    }

    std::vector<uint32_t> CachedShaderCompiler::compile_from_file(
        const std::string &path,
        const rhi::ShaderStage stage) {
        // Disk caching disabled — fall back to base class
        if (category_.empty()) {
            return ShaderCompiler::compile_from_file(path, stage);
        }

        // 1. Read source file
        const auto full_path = include_path().empty()
                                   ? std::filesystem::path(path)
                                   : std::filesystem::path(include_path()) / path;

        std::ifstream file(full_path);
        if (!file.is_open()) {
            spdlog::error("Failed to open shader file: {}", full_path.string());
            return {};
        }

        const std::string source{std::istreambuf_iterator(file), std::istreambuf_iterator<char>()};

        // 2. Compute disk key (includes path to avoid collisions between identical sources)
        const auto disk_key = compute_disk_key(source, stage, path);

        // 3. Try disk cache
        auto spirv = try_load_disk_cache(category_, disk_key, include_path());
        if (!spirv.empty()) {
            spdlog::info("Shader cache hit: {}", path);
            return spirv;
        }

        // 4. Disk miss — compile via parent (handles in-memory cache + shaderc)
        spirv = compile(source, stage, path);
        if (spirv.empty()) {
            return {};
        }

        // 5. Write back to disk cache
        if (const auto *entry = find_cache_entry(source, stage)) {
            write_disk_cache(category_, disk_key, spirv, entry->included_files);
        }

        return spirv;
    }
} // namespace himalaya::framework
