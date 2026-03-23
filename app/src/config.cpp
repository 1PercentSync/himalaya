/**
 * @file config.cpp
 * @brief AppConfig JSON persistence implementation.
 *
 * Config location: %LOCALAPPDATA%\himalaya\config.json
 * Uses nlohmann/json for serialization. All I/O errors are caught
 * and logged — never propagated as exceptions.
 */

#include <himalaya/app/config.h>

#include <fstream>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#ifdef _WIN32
#include <ShlObj.h>
#endif

namespace himalaya::app {
    std::filesystem::path config_file_path() {
        std::filesystem::path dir;

#ifdef _WIN32
        // LOCALAPPDATA via SHGetKnownFolderPath (Vista+)
        PWSTR wide_path = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &wide_path))) {
            dir = std::filesystem::path(wide_path) / "himalaya";
            CoTaskMemFree(wide_path);
        } else {
            // Fallback: executable directory
            spdlog::warn("Failed to resolve LOCALAPPDATA, using current directory for config");
            dir = std::filesystem::current_path() / "himalaya_config";
        }
#else
        // Non-Windows fallback (development convenience)
        dir = std::filesystem::current_path() / "himalaya_config";
#endif

        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        if (ec) {
            spdlog::warn("Failed to create config directory {}: {}", dir.string(), ec.message());
        }

        return dir / "config.json";
    }

    AppConfig load_config() {
        AppConfig config;

        try {
            const auto path = config_file_path();
            if (!std::filesystem::exists(path)) {
                spdlog::info("No config file found at {}, using defaults", path.string());
                return config;
            }

            std::ifstream file(path);
            if (!file.is_open()) {
                spdlog::warn("Failed to open config file {}", path.string());
                return config;
            }

            const auto json = nlohmann::json::parse(file);

            if (json.contains("scene_path") && json["scene_path"].is_string()) {
                config.scene_path = json["scene_path"].get<std::string>();
            }
            if (json.contains("env_path") && json["env_path"].is_string()) {
                config.env_path = json["env_path"].get<std::string>();
            }
            if (json.contains("hdr_sun_coords") && json["hdr_sun_coords"].is_object()) {
                for (auto& [key, val] : json["hdr_sun_coords"].items()) {
                    if (val.is_array() && val.size() == 2
                        && val[0].is_number_integer() && val[1].is_number_integer()) {
                        config.hdr_sun_coords[key] = {val[0].get<int>(), val[1].get<int>()};
                    }
                }
            }

            spdlog::info("Loaded config from {}", path.string());
        } catch (const std::exception& e) {
            spdlog::warn("Failed to load config: {}", e.what());
        }

        return config;
    }

    void save_config(const AppConfig& config) {
        try {
            const auto path = config_file_path();
            const auto tmp = path.parent_path() / "config.json.tmp";

            // Write to temp file first
            {
                std::ofstream file(tmp);
                if (!file.is_open()) {
                    spdlog::warn("Failed to open temp config file for writing: {}", tmp.string());
                    return;
                }

                nlohmann::json j;
                j["scene_path"] = config.scene_path;
                j["env_path"] = config.env_path;
                nlohmann::json coords = nlohmann::json::object();
                for (const auto& [path, xy] : config.hdr_sun_coords) {
                    coords[path] = {xy.first, xy.second};
                }
                j["hdr_sun_coords"] = coords;
                file << j.dump(2);
            }

            // Atomic replace (rename is atomic on NTFS)
            std::error_code ec;
            std::filesystem::rename(tmp, path, ec);
            if (ec) {
                spdlog::warn("Failed to rename config file: {}", ec.message());
                std::filesystem::remove(tmp, ec);
                return;
            }

            spdlog::info("Saved config to {}", path.string());
        } catch (const std::exception& e) {
            spdlog::warn("Failed to save config: {}", e.what());
        }
    }
} // namespace himalaya::app
