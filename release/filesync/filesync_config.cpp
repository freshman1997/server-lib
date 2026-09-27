#include "filesync_config.h"
#include "filesync_common.h"
#include "filesync_limits.h"
#include "nlohmann/json.hpp"

#include <fstream>
#include <stdexcept>

namespace filesync::config 
{
    namespace detail 
    {
        std::vector<std::string> strings(const nlohmann::json& object, const char* key) 
        {
            std::vector<std::string> result;
            if (!object.contains(key)) return result;
            if (!object.at(key).is_array()) throw std::runtime_error(std::string(key) + " must be an array");
            for (const auto& value : object.at(key)) {
                if (!value.is_string()) throw std::runtime_error(std::string(key) + " must contain strings");
                result.push_back(value.get<std::string>());
            }
            return result;
        }
    }

    model::Config load(const std::filesystem::path& path) 
    {
        std::ifstream input(path);
        if (!input) {
            throw std::runtime_error("cannot open config: " + path.string());
        }

        nlohmann::json json;
        input >> json;
        model::Config result;
        result.listen_host = json.value("listen_host", result.listen_host);
        const auto listen_port = json.value("listen_port", static_cast<int>(result.listen_port));
        const auto peer_port = json.value("peer_port", static_cast<int>(result.peer_port));
        if (listen_port < 1 || listen_port > 65535 || peer_port < 1 || peer_port > 65535) {
            throw std::runtime_error("ports must be between 1 and 65535");
        }

        result.listen_port = static_cast<std::uint16_t>(listen_port);
        result.peer_host = json.value("peer_host", result.peer_host);
        result.peer_port = static_cast<std::uint16_t>(peer_port);
        result.token = json.value("token", result.token);
        result.conflict_strategy = json.value("conflict_strategy", result.conflict_strategy);
        result.sync_mode = json.value("sync_mode", result.sync_mode);
        result.sync_deletes = json.value("sync_deletes", result.sync_deletes);
        result.scan_interval_ms = json.value("scan_interval_ms", result.scan_interval_ms);
        result.chunk_size = static_cast<std::size_t>(json.value("chunk_size", static_cast<int>(result.chunk_size)));

        if (result.chunk_size == 0 || result.chunk_size > limits::max_chunk_size) {
            throw std::runtime_error("chunk_size exceeds configured maximum");
        }

        result.include_extensions = detail::strings(json, "include_extensions");
        result.include_patterns = detail::strings(json, "include_patterns");
        result.exclude_patterns = detail::strings(json, "exclude_patterns");
        if (!json.contains("paths") || !json.at("paths").is_array() || json.at("paths").empty()) {
            throw std::runtime_error("config paths is empty");
        }

        for (const auto& item : json.at("paths")) {
            if (!item.is_object() || !item.contains("local") || !item.at("local").is_string()) {
                throw std::runtime_error("each path entry requires a string local");
            }

            const auto prefix = normalize_relative_path(item.value("remote_prefix", std::string{}));
            if (!prefix.empty() && !is_safe_relative_path(prefix)) {
                throw std::runtime_error("remote_prefix must be a safe relative path");
            }

            result.paths.push_back({item.at("local").get<std::string>(), prefix});
        }

        if (result.scan_interval_ms <= 0) {
            throw std::runtime_error("scan_interval_ms must be positive");
        }

        if (result.conflict_strategy != "keep_both" && result.conflict_strategy != "newer_wins") {
            throw std::runtime_error("invalid conflict_strategy");
        }

        if (result.sync_mode != "bidirectional" && result.sync_mode != "send_only" && result.sync_mode != "receive_only") {
            throw std::runtime_error("invalid sync_mode");
        }

        return result;
    }

    std::filesystem::path state_file(const model::Config& config) 
    {
        auto base = config.paths.front().local;
        if (std::filesystem::is_regular_file(base)) {
            base = base.parent_path();
        }

        return base / ".filesync_state.json";
    }

    model::Manifest load_state(const model::Config& config) 
    {
        model::Manifest result;
        std::ifstream input(state_file(config));
        if (!input) {
            return result;
        }

        try {
            nlohmann::json json; input >> json;
            if (!json.is_object() || !json.contains("manifest") || !json.at("manifest").is_array()) return result;
            for (const auto& item : json.at("manifest")) {
                if (!item.is_object() || !item.contains("path") || !item.at("path").is_string()) {
                    continue;
                }

                const auto path = item.at("path").get<std::string>();
                if (!is_safe_relative_path(path)) {
                    continue;
                }

                result[path] = {item.value("is_directory", false), item.value("size", 0ull), item.value("mtime", 0ull), item.value("hash", std::string{})};
            }
        } catch (...) { return {}; }

        return result;
    }

    void save_state(const model::Config& config, const model::Manifest& manifest) 
    {
        if (!config.sync_deletes) return;
        nlohmann::json json; json["manifest"] = nlohmann::json::array();
        for (const auto& entry : manifest) json["manifest"].push_back({{"path", entry.first}, {"is_directory", entry.second.is_directory}, {"size", entry.second.size}, {"mtime", entry.second.mtime}, {"hash", entry.second.hash}});
        const auto path = state_file(config);
        std::filesystem::create_directories(path.parent_path());
        const auto temporary = path.string() + ".tmp";
        {
            std::ofstream output(temporary, std::ios::trunc);
            if (!output) throw std::runtime_error("cannot write sync state: " + temporary);
            output << json.dump(2);
            if (!output) throw std::runtime_error("cannot flush sync state: " + temporary);
        }

        std::error_code ec;
        std::filesystem::rename(temporary, path, ec);
        if (ec) {
            std::filesystem::remove(path, ec);
            ec.clear();
            std::filesystem::rename(temporary, path, ec);
        }

        if (ec) {
            std::filesystem::remove(temporary, ec);
            throw std::runtime_error("cannot replace sync state: " + path.string());
        }
    }
}
