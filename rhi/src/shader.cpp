#include <himalaya/rhi/shader.h>
#include <himalaya/rhi/context.h>

#include <spdlog/spdlog.h>
#include <shaderc/shaderc.hpp>

#include <filesystem>
#include <fstream>

namespace himalaya::rhi {
    namespace {
        // Holds the lifetime of strings referenced by shaderc_include_result.
        struct IncludeResultData {
            std::string source_name;
            std::string content;
        };

        // Resolves #include directives by reading files from a configured root directory.
        // Relative includes (#include "...") resolve relative to the requesting file's directory.
        // Standard includes (#include <...>) resolve directly from the root. No fallback.
        // Collects all resolved files and their content for cache invalidation.
        class FileIncluder final : public shaderc::CompileOptions::IncluderInterface {
        public:
            explicit FileIncluder(std::filesystem::path root,
                                  std::vector<std::pair<std::string, std::string> > *included_files)
                : root_(std::move(root)), included_files_(included_files) {
            }

            shaderc_include_result *GetInclude(
                const char *requested_source,
                const shaderc_include_type type,
                const char *requesting_source,
                size_t /*include_depth*/) override {
                // Resolve path based on include type
                std::filesystem::path resolved;
                if (type == shaderc_include_type_relative) {
                    const auto parent = std::filesystem::path(requesting_source).parent_path();
                    resolved = parent / requested_source;
                } else {
                    resolved = requested_source;
                }
                resolved = resolved.lexically_normal();

                // Read from filesystem
                const auto full_path = root_ / resolved;
                std::ifstream file(full_path);

                auto *data = new IncludeResultData;
                auto *result = new shaderc_include_result{};
                result->user_data = data;

                if (!file.is_open()) {
                    data->content = "Failed to open include file: " + full_path.string();
                    result->source_name = "";
                    result->source_name_length = 0;
                    result->content = data->content.c_str();
                    result->content_length = data->content.size();
                    return result;
                }

                data->source_name = resolved.generic_string();
                data->content.assign(
                    std::istreambuf_iterator<char>(file),
                    std::istreambuf_iterator<char>());

                // Track included file for cache invalidation
                if (included_files_) {
                    included_files_->emplace_back(data->source_name, data->content);
                }

                result->source_name = data->source_name.c_str();
                result->source_name_length = data->source_name.size();
                result->content = data->content.c_str();
                result->content_length = data->content.size();
                return result;
            }

            void ReleaseInclude(shaderc_include_result *data) override {
                delete static_cast<IncludeResultData *>(data->user_data);
                delete data;
            }

        private:
            std::filesystem::path root_;
            std::vector<std::pair<std::string, std::string> > *included_files_;
        };
    } // anonymous namespace

    // Maps ShaderStage to the shaderc shader kind enum
    static shaderc_shader_kind to_shaderc_kind(const ShaderStage stage) {
        switch (stage) {
            case ShaderStage::Vertex: return shaderc_glsl_vertex_shader;
            case ShaderStage::Fragment: return shaderc_glsl_fragment_shader;
            case ShaderStage::Compute: return shaderc_glsl_compute_shader;
            case ShaderStage::RayGen: return shaderc_glsl_raygen_shader;
            case ShaderStage::ClosestHit: return shaderc_glsl_closesthit_shader;
            case ShaderStage::AnyHit: return shaderc_glsl_anyhit_shader;
            case ShaderStage::Miss: return shaderc_glsl_miss_shader;
        }
        std::abort();
    }

    // Builds a collision-free cache key: single-char stage prefix + full source text.
    static std::string make_cache_key(const std::string &source, const ShaderStage stage) {
        char prefix;
        switch (stage) {
            case ShaderStage::Vertex: prefix = 'V'; break;
            case ShaderStage::Fragment: prefix = 'F'; break;
            case ShaderStage::Compute: prefix = 'C'; break;
            case ShaderStage::RayGen: prefix = 'R'; break;
            case ShaderStage::ClosestHit: prefix = 'H'; break;
            case ShaderStage::AnyHit: prefix = 'A'; break;
            case ShaderStage::Miss: prefix = 'M'; break;
        }
        return prefix + source;
    }

    void ShaderCompiler::set_include_path(const std::string &path) {
        include_path_ = path;
    }

    const std::string &ShaderCompiler::include_path() const {
        return include_path_;
    }

    const ShaderCompiler::CacheEntry *ShaderCompiler::find_cache_entry(
        const std::string &source, const ShaderStage stage) const {
        const auto it = cache_.find(make_cache_key(source, stage));
        return it != cache_.end() ? &it->second : nullptr;
    }

    // Reads a shader file relative to include_path_ and delegates to compile().
    std::vector<uint32_t> ShaderCompiler::compile_from_file(
        const std::string &path,
        const ShaderStage stage) {
        const auto full_path = include_path_.empty()
            ? std::filesystem::path(path)
            : std::filesystem::path(include_path_) / path;

        std::ifstream file(full_path);
        if (!file.is_open()) {
            spdlog::error("Failed to open shader file: {}", full_path.string());
            return {};
        }

        std::string source{
            std::istreambuf_iterator<char>(file),
            std::istreambuf_iterator<char>()
        };

        return compile(source, stage, path);
    }

    // Validates that all files included during a previous compilation still have
    // the same content. Returns false if any file changed or cannot be read.
    static bool validate_includes(const std::filesystem::path &include_root,
                                  const std::vector<std::pair<std::string, std::string> > &included_files) {
        for (const auto &[path, cached_content]: included_files) {
            std::ifstream file(include_root / path);
            if (!file.is_open()) return false;
            std::string current{
                std::istreambuf_iterator<char>(file),
                std::istreambuf_iterator<char>()
            };
            if (current != cached_content) return false;
        }
        return true;
    }

    // Compiles GLSL to SPIR-V using shaderc, with include-aware in-memory caching.
    // Cache entries are invalidated when any transitively included file changes.
    // Debug: no optimization + debug info for RenderDoc shader source mapping.
    // Release: performance optimization for production shader quality.
    std::vector<uint32_t> ShaderCompiler::compile(
        const std::string &source,
        const ShaderStage stage,
        const std::string &filename) {
        auto key = make_cache_key(source, stage);

        // Check cache: main source match + verify included files unchanged
        if (const auto it = cache_.find(key); it != cache_.end()) {
            if (it->second.included_files.empty() || include_path_.empty() ||
                validate_includes(include_path_, it->second.included_files)) {
                spdlog::debug("Shader cache hit: {}", filename);
                return it->second.spirv;
            }
            spdlog::debug("Shader cache invalidated (include changed): {}", filename);
            cache_.erase(it);
        }

        const shaderc::Compiler compiler;
        shaderc::CompileOptions options;

        options.SetTargetEnvironment(shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_4);
#ifdef NDEBUG
        options.SetOptimizationLevel(shaderc_optimization_level_performance);
#else
        options.SetOptimizationLevel(shaderc_optimization_level_zero);
        options.SetGenerateDebugInfo();
#endif

        // Track included files for cache invalidation on future lookups
        std::vector<std::pair<std::string, std::string> > included_files;
        if (!include_path_.empty()) {
            options.SetIncluder(std::make_unique<FileIncluder>(include_path_, &included_files));
        }

        const auto result = compiler.CompileGlslToSpv(
            source,
            to_shaderc_kind(stage),
            filename.c_str(), options
        );

        if (result.GetCompilationStatus() != shaderc_compilation_status_success) {
            spdlog::error("Shader compilation failed ({}):\n{}", filename, result.GetErrorMessage());
            return {};
        }

        if (result.GetNumWarnings() > 0) {
            spdlog::warn("Shader compilation warnings ({}):\n{}", filename, result.GetErrorMessage());
        }

        spdlog::info("Shader compiled: {}", filename);

        std::vector spirv(result.cbegin(), result.cend());
        cache_[std::move(key)] = {spirv, std::move(included_files)};
        return spirv;
    }

    // Creates a VkShaderModule from pre-compiled SPIR-V bytecode.
    // The module is typically short-lived: created before pipeline creation
    // and destroyed immediately after.
    VkShaderModule create_shader_module(
        // ReSharper disable once CppParameterMayBeConst
        VkDevice device,
        const std::vector<uint32_t> &spirv) {
        VkShaderModuleCreateInfo create_info{};
        create_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        create_info.codeSize = spirv.size() * sizeof(uint32_t);
        create_info.pCode = spirv.data();

        VkShaderModule shader_module;
        VK_CHECK(vkCreateShaderModule(device, &create_info, nullptr, &shader_module));

        return shader_module;
    }
} // namespace himalaya::rhi
