#include <himalaya/framework/cache.h>

#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <unordered_set>
#include <vector>

#include <spdlog/spdlog.h>
#include <xxhash.h>

namespace himalaya::framework {

    // ---- Helpers ----

    /// Formats a 128-bit XXH3 hash as a 32-char lowercase hex string.
    static std::string hash_to_hex(const XXH128_hash_t hash) {
        std::ostringstream oss;
        oss << std::hex << std::setfill('0')
            << std::setw(16) << hash.high64
            << std::setw(16) << hash.low64;
        return oss.str();
    }

    // ---- Public API ----

    std::filesystem::path cache_root() {
        // Windows: %TEMP%\himalaya\  (e.g. C:\Users\<user>\AppData\Local\Temp\himalaya\)
        // Static local: directory created once, thread-safe per C++11.
        static const auto root = [] {
            auto p = std::filesystem::temp_directory_path() / "himalaya";
            std::filesystem::create_directories(p);
            return p;
        }();
        return root;
    }

    std::string content_hash(const void *data, const size_t size) {
        const auto h = XXH3_128bits(data, size);
        return hash_to_hex(h);
    }

    std::string content_hash(const std::filesystem::path &file) {
        std::ifstream ifs(file, std::ios::binary | std::ios::ate);
        if (!ifs) {
            spdlog::warn("cache: cannot open file for hashing: {}", file.string());
            return {};
        }

        const auto file_size = static_cast<size_t>(ifs.tellg());
        if (file_size == 0) {
            // Hash of empty data — deterministic
            return content_hash(nullptr, 0);
        }

        ifs.seekg(0);
        std::vector<uint8_t> buf(file_size);
        ifs.read(reinterpret_cast<char *>(buf.data()), static_cast<std::streamsize>(file_size));
        return content_hash(buf.data(), buf.size());
    }

    // Shared state for cache_path() directory creation tracking.
    // clear_cache/clear_all_cache must also access this to invalidate entries.
    static std::mutex s_created_mtx;
    static std::unordered_set<std::string> s_created_categories;

    void clear_cache(const std::string_view category) {
        std::error_code ec;
        const auto dir = cache_root() / category;
        if (!std::filesystem::exists(dir, ec)) return;
        const auto removed = std::filesystem::remove_all(dir, ec);
        if (ec) {
            spdlog::warn("cache: failed to clear '{}': {}", dir.string(), ec.message());
        } else {
            std::lock_guard lock(s_created_mtx);
            s_created_categories.erase(std::string(category));
            spdlog::info("cache: cleared '{}' ({} entries removed)",
                         std::string(category), removed);
        }
    }

    void clear_all_cache() {
        std::error_code ec;
        const auto root = cache_root();
        if (!std::filesystem::exists(root, ec)) return;
        const auto removed = std::filesystem::remove_all(root, ec);
        if (ec) {
            spdlog::warn("cache: failed to clear root: {}", ec.message());
        } else {
            std::lock_guard lock(s_created_mtx);
            s_created_categories.clear();
            spdlog::info("cache: cleared all ({} entries removed)", removed);
        }
    }

    std::filesystem::path cache_path(const std::string_view category,
                                     const std::string_view hash,
                                     const std::string_view extension) {

        // Ensure category subdirectory exists (recreated after clear).
        const auto dir = cache_root() / category;
        {
            std::lock_guard lock(s_created_mtx);
            if (s_created_categories.insert(std::string(category)).second) {
                std::filesystem::create_directories(dir);
            }
        }
        return dir / (std::string(hash) + std::string(extension));
    }

    bool atomic_write_file(const std::filesystem::path &path,
                           const void *data, const size_t size) {
        auto tmp_path = path;
        tmp_path += ".tmp";

        std::ofstream ofs(tmp_path, std::ios::binary | std::ios::trunc);
        if (!ofs) {
            spdlog::error("atomic_write_file: cannot open: {}", tmp_path.string());
            return false;
        }
        ofs.write(static_cast<const char *>(data), static_cast<std::streamsize>(size));
        ofs.close();
        if (!ofs.good()) {
            spdlog::error("atomic_write_file: write failed: {}", path.string());
            std::error_code ec;
            std::filesystem::remove(tmp_path, ec);
            return false;
        }

        std::error_code ec;
        std::filesystem::rename(tmp_path, path, ec);
        if (ec) {
            spdlog::error("atomic_write_file: rename failed: {}", ec.message());
            std::filesystem::remove(tmp_path, ec);
            return false;
        }
        return true;
    }

} // namespace himalaya::framework
