#pragma once

/**
 * @file cache.h
 * @brief Shared cache infrastructure: directory, content hashing, and path management.
 */

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace himalaya::framework {

    /**
     * @brief Returns the cache root directory (%TEMP%\himalaya\).
     *
     * Creates the directory on first call. All cache consumers (textures, IBL)
     * store files under this root.
     */
    std::filesystem::path cache_root();

    /**
     * @brief Computes content hash of in-memory data (XXH3_128).
     * @return 32-character lowercase hexadecimal string.
     */
    std::string content_hash(const void *data, size_t size);

    /**
     * @brief Computes content hash of a file by reading its entire contents.
     * @return 32-character hex string, or empty string on read failure.
     */
    std::string content_hash(const std::filesystem::path &file);

    /**
     * @brief Builds a cache file path: cache_root() / category / (hash + extension).
     *
     * Creates the category subdirectory if it does not exist.
     *
     * @param category Subdirectory name (e.g. "textures", "ibl").
     * @param hash     Content hash string used as filename stem.
     * @param extension File extension including dot (e.g. ".ktx2").
     */
    std::filesystem::path cache_path(std::string_view category,
                                     std::string_view hash,
                                     std::string_view extension);

} // namespace himalaya::framework
