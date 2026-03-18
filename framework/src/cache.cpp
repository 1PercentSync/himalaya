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

    std::filesystem::path cache_path(const std::string_view category,
                                     const std::string_view hash,
                                     const std::string_view extension) {
        // Ensure category subdirectory exists (once per unique category).
        static std::mutex mtx;
        static std::unordered_set<std::string> created;

        const auto dir = cache_root() / category;
        {
            std::lock_guard lock(mtx);
            if (created.insert(std::string(category)).second) {
                std::filesystem::create_directories(dir);
            }
        }
        return dir / (std::string(hash) + std::string(extension));
    }

} // namespace himalaya::framework
