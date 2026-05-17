#include "filesync_common.h"

#include "buffer/byte_buffer.h"
#include "coroutine/task.h"
#include "net/runtime/network_runtime.h"
#include "net/session/stream_client_session.h"
#include "net/session/stream_server_session.h"
#include "nlohmann/json.hpp"

#include <map>
#include <mutex>
#include <set>
#include <iomanip>
#include <ctime>
#include <cctype>
#include <regex>
#include <unordered_map>
#include <optional>
#include <memory>

namespace {

constexpr std::size_t kControlFlushThreshold = 64 * 1024;

struct SyncPath {
    std::filesystem::path local;
    std::string remote_prefix;
};

struct Config {
    std::string listen_host = "0.0.0.0";
    uint16_t listen_port = 9095;
    std::string peer_host;
    uint16_t peer_port = 9095;
    std::string token = "change-me";
    std::string conflict_strategy = "keep_both";
    bool sync_deletes = false;
    int scan_interval_ms = 1000;
    std::size_t chunk_size = 32 * 1024;
    std::vector<std::string> include_extensions;
    std::vector<std::string> include_patterns;
    std::vector<std::string> exclude_patterns;
    std::vector<SyncPath> paths;
};

struct FileState {
    bool is_directory = false;
    std::uintmax_t size = 0;
    std::uint64_t mtime = 0;
    std::uint64_t hash = 0;
    std::filesystem::path local_path;

    bool operator==(const FileState& other) const {
        return is_directory == other.is_directory && size == other.size && mtime == other.mtime && hash == other.hash;
    }
};

using Manifest = std::map<std::string, FileState>;

struct HashCacheEntry {
    std::uintmax_t size = 0;
    std::uint64_t mtime = 0;
    std::uint64_t hash = 0;
};

std::mutex g_hash_cache_mutex;
std::unordered_map<std::string, HashCacheEntry> g_hash_cache;

std::vector<std::string> json_string_array(const nlohmann::json& json, const char* key) {
    std::vector<std::string> values;
    if (!json.contains(key)) {
        return values;
    }
    const auto& node = json.at(key);
    if (!node.is_array()) {
        return values;
    }
    for (const auto& item : node) {
        if (item.is_string()) {
            values.push_back(item.get<std::string>());
        }
    }
    return values;
}

Config load_config(const std::filesystem::path& path) {
    std::ifstream in(path);
    if (!in) {
        throw std::runtime_error("cannot open config: " + path.string());
    }
    nlohmann::json json;
    in >> json;

    Config config;
    config.listen_host = json.value("listen_host", config.listen_host);
    config.listen_port = static_cast<uint16_t>(json.value("listen_port", static_cast<int>(config.listen_port)));
    config.peer_host = json.value("peer_host", config.peer_host);
    config.peer_port = static_cast<uint16_t>(json.value("peer_port", static_cast<int>(config.peer_port)));
    config.token = json.value("token", config.token);
    config.conflict_strategy = json.value("conflict_strategy", config.conflict_strategy);
    config.sync_deletes = json.value("sync_deletes", config.sync_deletes);
    config.scan_interval_ms = json.value("scan_interval_ms", config.scan_interval_ms);
    config.chunk_size = static_cast<std::size_t>(json.value("chunk_size", static_cast<int>(config.chunk_size)));
    if (config.chunk_size == 0 || config.chunk_size > 32 * 1024) {
        throw std::runtime_error("chunk_size must be between 1 and 32768 bytes");
    }
    config.include_extensions = json_string_array(json, "include_extensions");
    config.include_patterns = json_string_array(json, "include_patterns");
    config.exclude_patterns = json_string_array(json, "exclude_patterns");
    if (json.contains("paths") && json.at("paths").is_array()) {
        for (const auto& item : json.at("paths")) {
            if (!item.contains("local") || !item.at("local").is_string()) {
                throw std::runtime_error("each path entry requires a string 'local'");
            }
            const auto remote_prefix = filesync::normalize_relative_path(item.value("remote_prefix", ""));
            if (!remote_prefix.empty() && !filesync::is_safe_relative_path(remote_prefix)) {
                throw std::runtime_error("remote_prefix must be a safe relative path");
            }
            config.paths.push_back({item.at("local").get<std::string>(), remote_prefix});
        }
    }
    if (config.paths.empty()) {
        throw std::runtime_error("config paths is empty");
    }

    if (config.scan_interval_ms <= 0) {
        throw std::runtime_error("scan_interval_ms must be positive");
    }

    if (config.conflict_strategy != "keep_both" && config.conflict_strategy != "newer_wins") {
        throw std::runtime_error("conflict_strategy must be keep_both or newer_wins");
    }

    return config;
}

std::string lowercase(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

std::string current_timestamp() {
    const auto now = std::chrono::system_clock::now();
    const auto time = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &time);
#else
    localtime_r(&time, &tm);
#endif
    std::ostringstream out;
    out << std::put_time(&tm, "%Y%m%d%H%M%S");
    return out.str();
}

std::string make_remote_path(const SyncPath& sync_path, const std::filesystem::path& relative) {
    const auto rel = filesync::normalize_relative_path(relative);
    if (sync_path.remote_prefix.empty()) {
        return rel;
    }
    if (rel.empty()) {
        return sync_path.remote_prefix;
    }
    return sync_path.remote_prefix + "/" + rel;
}

std::filesystem::path local_path_for_remote(const Config& config, const std::string& remote) {
    if (!filesync::is_safe_relative_path(remote)) {
        throw std::runtime_error("unsafe remote path: " + remote);
    }
    for (const auto& sync_path : config.paths) {
        const auto prefix = filesync::normalize_relative_path(sync_path.remote_prefix);
        if (std::filesystem::is_regular_file(sync_path.local)) {
            const auto mapped = prefix.empty() ? sync_path.local.filename().generic_string() : prefix;
            if (remote == mapped) return sync_path.local;
            continue;
        }
        if (prefix.empty()) {
            return sync_path.local / filesync::path_from_utf8(remote);
        }
        if (remote == prefix) {
            return sync_path.local;
        }
        const auto marker = prefix + "/";
        if (remote.rfind(marker, 0) == 0) {
            return sync_path.local / filesync::path_from_utf8(remote.substr(marker.size()));
        }
    }
    throw std::runtime_error("cannot map remote path: " + remote);
}

std::filesystem::path conflict_path_for(const std::filesystem::path& target) {
    auto candidate = target;
    candidate += ".conflict.";
    candidate += current_timestamp();
    for (int index = 1; std::filesystem::exists(candidate); ++index) {
        candidate = target;
        candidate += ".conflict.";
        candidate += current_timestamp();
        candidate += ".";
        candidate += std::to_string(index);
    }
    return candidate;
}

bool is_internal_file(const std::filesystem::path& path) {
    const auto name = path.filename().generic_string();
    return name == ".filesync_state.json" || name.find(".conflict.") != std::string::npos ||
        (name.size() >= 13 && name.rfind(".filesync.tmp") == name.size() - 13);
}

std::string normalize_filter_pattern(std::string pattern) {
    std::replace(pattern.begin(), pattern.end(), '\\', '/');
    while (pattern.rfind("./", 0) == 0) {
        pattern.erase(0, 2);
    }
    while (!pattern.empty() && pattern.front() == '/') {
        pattern.erase(pattern.begin());
    }
    while (pattern.size() > 1 && pattern.back() == '/') {
        pattern.pop_back();
    }
    return pattern;
}

std::string glob_to_regex(const std::string& glob) {
    const auto normalized = normalize_filter_pattern(glob);
    std::string out = "^";
    for (std::size_t i = 0; i < normalized.size(); ++i) {
        const char ch = normalized[i];
        if (ch == '/' && i + 2 < normalized.size() && normalized[i + 1] == '*' && normalized[i + 2] == '*' &&
            i + 3 == normalized.size()) {
            out += "(?:/.*)?";
            i += 2;
            continue;
        }
        if (ch == '*') {
            if (i + 1 < normalized.size() && normalized[i + 1] == '*') {
                if (i + 2 < normalized.size() && normalized[i + 2] == '/') {
                    out += "(?:.*/)?";
                    i += 2;
                } else {
                    out += ".*";
                    ++i;
                }
            } else {
                out += "[^/]*";
            }
        } else if (ch == '?') {
            out += "[^/]";
        } else {
            if (std::string_view(".^$+()[]{}|\\").find(ch) != std::string_view::npos) {
                out.push_back('\\');
            }
            out.push_back(ch);
        }
    }
    out += "$";
    return out;
}

bool glob_match(const std::string& pattern, const std::string& value) {
    try {
        return std::regex_match(value, std::regex(glob_to_regex(pattern), std::regex::ECMAScript | std::regex::icase));
    } catch (const std::regex_error&) {
        return false;
    }
}

std::string basename_of(const std::string& value) {
    const auto pos = value.find_last_of('/');
    return pos == std::string::npos ? value : value.substr(pos + 1);
}

std::vector<std::string> path_and_ancestors(const std::string& value) {
    std::vector<std::string> paths;
    auto current = filesync::normalize_relative_path(value);
    while (!current.empty()) {
        paths.push_back(current);
        const auto pos = current.find_last_of('/');
        if (pos == std::string::npos) {
            break;
        }
        current.erase(pos);
    }
    return paths;
}

std::vector<std::string> filter_candidates_for_remote(const Config& config, const std::string& remote) {
    std::vector<std::string> candidates{filesync::normalize_relative_path(remote)};
    for (const auto& sync_path : config.paths) {
        const auto prefix = filesync::normalize_relative_path(sync_path.remote_prefix);
        if (prefix.empty()) {
            continue;
        }
        const auto marker = prefix + "/";
        if (remote.rfind(marker, 0) == 0) {
            candidates.push_back(remote.substr(marker.size()));
        }
    }
    std::sort(candidates.begin(), candidates.end());
    candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());
    return candidates;
}

bool pattern_matches_path(const std::string& raw_pattern, const std::string& value) {
    const auto pattern = normalize_filter_pattern(raw_pattern);
    if (pattern.empty()) {
        return false;
    }
    const bool pattern_has_slash = pattern.find('/') != std::string::npos;
    for (const auto& candidate : path_and_ancestors(value)) {
        if (glob_match(pattern, candidate)) {
            return true;
        }
        if (!pattern_has_slash && glob_match(pattern, basename_of(candidate))) {
            return true;
        }
    }
    return false;
}

bool any_pattern_matches(const std::vector<std::string>& patterns, const std::vector<std::string>& candidates) {
    return std::any_of(patterns.begin(), patterns.end(), [&](const std::string& pattern) {
        return std::any_of(candidates.begin(), candidates.end(), [&](const std::string& candidate) {
            return pattern_matches_path(pattern, candidate);
        });
    });
}

bool extension_allowed(const Config& config, const std::filesystem::path& path) {
    if (config.include_extensions.empty()) {
        return true;
    }
    auto ext = lowercase(path.extension().generic_string());
    return std::any_of(config.include_extensions.begin(), config.include_extensions.end(), [&](std::string allowed) {
        allowed = lowercase(std::move(allowed));
        if (!allowed.empty() && allowed.front() != '.') {
            allowed.insert(allowed.begin(), '.');
        }
        return ext == allowed;
    });
}

bool should_include_path(const Config& config,
                         const std::filesystem::path& local,
                         const std::string& remote,
                         bool is_directory) {
    if (is_internal_file(local) || is_internal_file(filesync::path_from_utf8(remote))) {
        return false;
    }
    if (!is_directory && !extension_allowed(config, local)) {
        return false;
    }
    const auto candidates = filter_candidates_for_remote(config, remote);
    if (!config.include_patterns.empty() && !any_pattern_matches(config.include_patterns, candidates)) {
        return false;
    }
    if (any_pattern_matches(config.exclude_patterns, candidates)) {
        return false;
    }
    return true;
}

std::uint64_t cached_file_hash(const std::filesystem::path& path, std::uintmax_t size, std::uint64_t mtime) {
    const auto key = filesync::path_to_utf8(path);
    {
        std::lock_guard<std::mutex> lock(g_hash_cache_mutex);
        const auto it = g_hash_cache.find(key);
        if (it != g_hash_cache.end() && it->second.size == size && it->second.mtime == mtime) {
            return it->second.hash;
        }
    }

    const auto hash = filesync::fnv1a_file_hash(path);
    {
        std::lock_guard<std::mutex> lock(g_hash_cache_mutex);
        g_hash_cache[key] = {size, mtime, hash};
    }
    return hash;
}

bool should_include_remote_path(const Config& config, const std::string& remote, bool is_directory) {
    const auto remote_path = filesync::path_from_utf8(remote);
    if (is_internal_file(remote_path)) {
        return false;
    }
    if (!is_directory && !extension_allowed(config, remote_path)) {
        return false;
    }
    const auto candidates = filter_candidates_for_remote(config, remote);
    if (!config.include_patterns.empty() && !any_pattern_matches(config.include_patterns, candidates)) {
        return false;
    }
    if (any_pattern_matches(config.exclude_patterns, candidates)) {
        return false;
    }
    return true;
}

Manifest scan_paths(const Config& config) {
    Manifest manifest;
    for (const auto& sync_path : config.paths) {
        if (!std::filesystem::exists(sync_path.local)) {
            std::filesystem::create_directories(sync_path.local);
        }
        if (std::filesystem::is_regular_file(sync_path.local)) {
            const auto remote = sync_path.remote_prefix.empty()
                ? sync_path.local.filename().generic_string()
                : filesync::normalize_relative_path(sync_path.remote_prefix);
            if (!should_include_path(config, sync_path.local, remote, false)) {
                continue;
            }
            manifest[remote] = {
                false,
                std::filesystem::file_size(sync_path.local),
                filesync::file_time_to_seconds(std::filesystem::last_write_time(sync_path.local)),
                0,
                sync_path.local,
            };
            manifest[remote].hash = cached_file_hash(sync_path.local, manifest[remote].size, manifest[remote].mtime);
            continue;
        }
        for (auto it = std::filesystem::recursive_directory_iterator(sync_path.local);
             it != std::filesystem::recursive_directory_iterator();
             ++it) {
            const auto& entry = *it;
            const auto relative = std::filesystem::relative(entry.path(), sync_path.local);
            const auto remote = make_remote_path(sync_path, relative);
            if (entry.is_directory()) {
                if (!should_include_path(config, entry.path(), remote, true)) {
                    it.disable_recursion_pending();
                    continue;
                }
                manifest[remote] = {true, 0, 0, 0, entry.path()};
            } else if (entry.is_regular_file()) {
                if (!should_include_path(config, entry.path(), remote, false)) {
                    continue;
                }
                const auto size = entry.file_size();
                const auto mtime = filesync::file_time_to_seconds(entry.last_write_time());
                manifest[remote] = {
                    false,
                    size,
                    mtime,
                    cached_file_hash(entry.path(), size, mtime),
                    entry.path(),
                };
            }
        }
    }
    return manifest;
}

bool should_pull(const Manifest& local, const std::string& path, const FileState& remote) {
    const auto it = local.find(path);
    if (it == local.end()) return true;
    if (remote.is_directory) return false;
    const auto& current = it->second;
    if (current.is_directory) return true;
    if (current.size == remote.size && current.hash == remote.hash) return false;
    if (remote.mtime > current.mtime) return true;
    return remote.mtime == current.mtime && remote.hash != current.hash;
}

std::filesystem::path state_file_path(const Config& config) {
    if (config.paths.empty()) {
        return ".filesync_state.json";
    }
    auto base = config.paths.front().local;
    if (std::filesystem::is_regular_file(base)) {
        base = base.parent_path();
    }
    return base / ".filesync_state.json";
}

void save_state(const Config& config, const Manifest& manifest) {
    if (!config.sync_deletes) {
        return;
    }
    nlohmann::json json = nlohmann::json::object();
    json["manifest"] = nlohmann::json::array();
    for (const auto& [path, state] : manifest) {
        json["manifest"].push_back({
            {"path", path},
            {"is_directory", state.is_directory},
            {"size", state.size},
            {"mtime", state.mtime},
            {"hash", state.hash},
        });
    }
    const auto path = state_file_path(config);
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::trunc);
    out << json.dump(2);
}

Manifest load_state(const Config& config) {
    Manifest manifest;
    const auto state_path = state_file_path(config);
    std::ifstream in(state_path);
    if (!in) {
        return manifest;
    }
    try {
        nlohmann::json json;
        in >> json;

        if (!json.is_object()) {
            return manifest;
        }

        const auto it = json.find("manifest");
        if (it == json.end() || !it->is_array()) {
            return manifest;
        }

        for (const auto& item : *it) {
            if (!item.is_object()) {
                continue;
            }

            const auto path_it = item.find("path");
            if (path_it == item.end() || !path_it->is_string()) {
                continue;
            }
            const auto path = path_it->get<std::string>();
            if (!filesync::is_safe_relative_path(path)) {
                continue;
            }

            const bool is_directory = item.value("is_directory", false);
            const auto size = static_cast<std::uintmax_t>(item.value("size", 0ull));
            const auto mtime = static_cast<std::uint64_t>(item.value("mtime", 0ull));
            const auto hash = static_cast<std::uint64_t>(item.value("hash", 0ull));
            manifest[path] = {is_directory, size, mtime, hash};
        }
    } catch (const std::exception& ex) {
        std::cerr << "warning: failed to load state file " << state_path.string() << ": " << ex.what() << "\n";
        return {};
    }

    return manifest;
}

std::vector<std::string> deleted_paths(const Config& config, const Manifest& remote, const Manifest& local) {
    std::vector<std::string> paths;
    if (!config.sync_deletes) {
        return paths;
    }
    const auto previous = load_state(config);
    if (previous.empty()) {
        return paths;
    }
    for (const auto& [path, previous_state] : previous) {
        (void)previous_state;
        if (local.find(path) == local.end() && remote.find(path) != remote.end()) {
            paths.push_back(path);
        }
    }
    std::sort(paths.begin(), paths.end(), [&](const std::string& a, const std::string& b) {
        return std::filesystem::path(a).string().size() > std::filesystem::path(b).string().size();
    });
    return paths;
}

std::string manifest_message(const Config& config, const Manifest& manifest) {
    std::ostringstream out;
    out << "HELLO filesync/2 " << filesync::quote_token(config.token) << "\n";
    out << "MANIFEST " << manifest.size() << "\n";
    for (const auto& [path, state] : manifest) {
        out << (state.is_directory ? "D " : "F ") << filesync::quote_token(path) << " " << state.size << " "
            << state.mtime << " " << state.hash << "\n";
    }
    out << "END\n";
    return out.str();
}

void write_text(yuan::net::ConnectionContext& ctx, const std::string& text);
void write_text(yuan::net::StreamClientSession& session, const std::string& text);

template <typename Writer>
void write_manifest_to(Writer& writer, const Config& config, const Manifest& manifest) {
    std::string text;
    text.reserve(kControlFlushThreshold);
    auto flush_text = [&]() {
        if (!text.empty()) {
            write_text(writer, text);
            text.clear();
        }
    };
    auto append_line = [&](std::string line) {
        text += std::move(line);
        text.push_back('\n');
        if (text.size() >= kControlFlushThreshold) {
            flush_text();
        }
    };

    append_line("HELLO filesync/2 " + filesync::quote_token(config.token));
    append_line("MANIFEST " + std::to_string(manifest.size()));
    for (const auto& [path, state] : manifest) {
        append_line(std::string(state.is_directory ? "D " : "F ") + filesync::quote_token(path) + " " +
                    std::to_string(state.size) + " " + std::to_string(state.mtime) + " " +
                    std::to_string(state.hash));
    }
    append_line("END");
    flush_text();
}

void write_manifest(yuan::net::ConnectionContext& ctx, const Config& config, const Manifest& manifest) {
    write_manifest_to(ctx, config, manifest);
}

void write_manifest(yuan::net::StreamClientSession& session, const Config& config, const Manifest& manifest) {
    write_manifest_to(session, config, manifest);
}

void write_text(yuan::net::ConnectionContext& ctx, const std::string& text) {
    yuan::buffer::ByteBuffer buffer{std::string_view(text)};
    ctx.write_and_flush(buffer);
}

void write_text(yuan::net::StreamClientSession& session, const std::string& text) {
    yuan::buffer::ByteBuffer buffer{std::string_view(text)};
    session.write_and_flush(buffer);
}

std::string read_buffer_text(yuan::net::ConnectionContext& ctx) {
    const auto buffer = ctx.take_input_byte_buffer();
    const auto span = buffer.readable_span();
    return std::string(span.begin(), span.end());
}

Manifest parse_manifest(const std::vector<std::string>& lines, const Config& config) {
    if (lines.size() < 3) throw std::runtime_error("bad manifest");
    const auto hello = filesync::split_line(lines[0]);
    if (hello.size() != 3 || hello[0] != "HELLO" || hello[1] != "filesync/2" ||
        filesync::unquote_token(hello[2]) != config.token) {
        throw std::runtime_error("unauthorized peer");
    }
    Manifest manifest;
    for (std::size_t i = 2; i + 1 < lines.size(); ++i) {
        const auto parts = filesync::split_line(lines[i]);
        if (parts.size() != 5) continue;
        const auto path = filesync::unquote_token(parts[1]);
        if (!filesync::is_safe_relative_path(path)) continue;
        manifest[path] = {
            parts[0] == "D",
            static_cast<std::uintmax_t>(std::stoull(parts[2])),
            static_cast<std::uint64_t>(std::stoull(parts[3])),
            static_cast<std::uint64_t>(std::stoull(parts[4])),
        };
    }
    return manifest;
}

std::vector<std::string> split_lines(const std::string& text) {
    std::istringstream in(text);
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(line);
    }
    return lines;
}

bool extract_next_frame(std::string& buffer, std::string& frame) {
    static const std::string marker = "\nEND\n";
    const auto pos = buffer.find(marker);
    if (pos == std::string::npos) {
        return false;
    }
    frame = buffer.substr(0, pos + marker.size());
    buffer.erase(0, pos + marker.size());
    return true;
}

std::string need_message(const Config& config, const Manifest& remote) {
    const auto local = scan_paths(config);
    std::vector<std::string> needed;
    const auto deleted = deleted_paths(config, remote, local);
    std::set<std::string> deleted_set(deleted.begin(), deleted.end());
    for (const auto& [path, state] : remote) {
        if (!should_include_remote_path(config, path, state.is_directory)) {
            continue;
        }
        if (deleted_set.find(path) != deleted_set.end()) {
            continue;
        }
        if (should_pull(local, path, state)) {
            needed.push_back(path);
        }
    }
    std::ostringstream out;
    out << "NEED " << (deleted.size() + needed.size()) << "\n";
    for (const auto& path : deleted) {
        out << "DELETE " << filesync::quote_token(path) << "\n";
    }
    for (const auto& path : needed) {
        out << "GET " << filesync::quote_token(path) << "\n";
    }
    out << "END\n";
    return out.str();
}

void apply_delete(const Config& config, const std::string& path) {
    if (!config.sync_deletes || !filesync::is_safe_relative_path(path)) {
        return;
    }
    const auto target = local_path_for_remote(config, path);
    if (!std::filesystem::exists(target)) {
        return;
    }
    std::error_code type_ec;
    const bool is_directory = std::filesystem::is_directory(target, type_ec);
    if (!should_include_remote_path(config, path, is_directory)) {
        return;
    }
    std::error_code ec;
    if (is_directory) {
        std::filesystem::remove_all(target, ec);
    } else {
        std::filesystem::remove(target, ec);
    }
    if (ec) {
        std::cerr << "failed to delete " << path << ": " << ec.message() << "\n";
    } else {
        std::cout << "deleted " << path << "\n";
    }
}

std::set<std::string> parse_need(const std::string& text) {
    const auto lines = split_lines(text);
    std::set<std::string> needed;
    for (std::size_t i = 1; i + 1 < lines.size(); ++i) {
        const auto parts = filesync::split_line(lines[i]);
        if (parts.size() == 2 && parts[0] == "DELETE") {
            continue;
        }
        if (parts.size() == 2 && parts[0] == "GET") {
            const auto path = filesync::unquote_token(parts[1]);
            if (filesync::is_safe_relative_path(path)) {
                needed.insert(path);
            }
        } else if (parts.size() == 1) {
            const auto path = filesync::unquote_token(parts[0]);
            if (filesync::is_safe_relative_path(path)) {
                needed.insert(path);
            }
        }
    }
    return needed;
}

void apply_need_deletes(const Config& config, const std::string& text) {
    const auto lines = split_lines(text);
    for (std::size_t i = 1; i + 1 < lines.size(); ++i) {
        const auto parts = filesync::split_line(lines[i]);
        if (parts.size() == 2 && parts[0] == "DELETE") {
            apply_delete(config, filesync::unquote_token(parts[1]));
        }
    }
}

bool is_ready_frame(const std::string& text) {
    const auto lines = split_lines(text);
    if (lines.empty()) {
        return false;
    }
    const auto parts = filesync::split_line(lines.front());
    return !parts.empty() && parts.front() == "READY";
}

bool is_conflict(const Config& config,
                 const Manifest& last_manifest,
                 bool have_last_manifest,
                 const std::string& path,
                 const FileState& remote_state,
                 const std::filesystem::path& target) {
    (void)last_manifest;
    (void)have_last_manifest;
    (void)path;
    if (config.conflict_strategy != "keep_both" || !std::filesystem::exists(target) ||
        !std::filesystem::is_regular_file(target)) {
        return false;
    }

    const FileState current{
        false,
        std::filesystem::file_size(target),
        filesync::file_time_to_seconds(std::filesystem::last_write_time(target)),
        filesync::fnv1a_file_hash(target),
    };
    if (current.hash == remote_state.hash && current.size == remote_state.size) {
        return false;
    }
    return true;
}

std::string format_percent(double value) {
    if (value < 0.0) value = 0.0;
    if (value > 100.0) value = 100.0;
    std::ostringstream out;
    out << std::fixed << std::setprecision(1) << value << "%";
    return out.str();
}

std::string format_bytes(std::uintmax_t bytes) {
    static constexpr const char* units[] = {"B", "KB", "MB", "GB", "TB"};
    double value = static_cast<double>(bytes);
    std::size_t unit = 0;
    while (value >= 1024.0 && unit + 1 < std::size(units)) {
        value /= 1024.0;
        ++unit;
    }
    std::ostringstream out;
    if (unit == 0) {
        out << bytes << units[unit];
    } else {
        out << std::fixed << std::setprecision(1) << value << units[unit];
    }
    return out.str();
}

double percent_of(std::uintmax_t done, std::uintmax_t total) {
    if (total == 0) {
        return 100.0;
    }
    return (static_cast<double>(done) * 100.0) / static_cast<double>(total);
}

double percent_of(double done, double total) {
    if (total <= 0.0) {
        return 100.0;
    }
    return (done * 100.0) / total;
}

void print_sync_progress(const std::string& phase,
                         double total_percent,
                         const std::string& path,
                         double file_percent,
                         std::uintmax_t file_done,
                         std::uintmax_t file_total) {
    std::cout << "sync " << phase << " total=" << format_percent(total_percent)
              << " file=" << filesync::quote_token(path)
              << " file_progress=" << format_percent(file_percent)
              << " (" << format_bytes(file_done) << "/" << format_bytes(file_total) << ")" << std::endl;
}

struct ProgressThrottle {
    int last_bucket = -1;
    std::chrono::steady_clock::time_point last_report = std::chrono::steady_clock::now() - std::chrono::seconds(2);

    bool should_report(std::uintmax_t done, std::uintmax_t total, bool force) {
        if (force) {
            last_bucket = 100;
            last_report = std::chrono::steady_clock::now();
            return true;
        }
        if (total > 0 && done >= total) {
            return false;
        }
        const auto percent = total == 0 ? 100.0 : percent_of(done, total);
        const int bucket = static_cast<int>(percent / 5.0);
        const auto now = std::chrono::steady_clock::now();
        if (bucket != last_bucket || now - last_report >= std::chrono::seconds(1)) {
            last_bucket = bucket;
            last_report = now;
            return true;
        }
        return false;
    }

    void reset() {
        last_bucket = -1;
        last_report = std::chrono::steady_clock::now() - std::chrono::seconds(2);
    }
};

struct SendProgress {
    std::size_t total_files = 0;
    std::size_t completed_files = 0;
    std::uintmax_t total_bytes = 0;
    std::uintmax_t completed_bytes = 0;
    ProgressThrottle file_throttle;

    double total_percent(std::uintmax_t current_file_done = 0) const {
        if (total_bytes > 0) {
            return percent_of(completed_bytes + current_file_done, total_bytes);
        }
        return percent_of(static_cast<double>(completed_files), static_cast<double>(total_files));
    }
};

template <typename Writer>
void write_file_lines_to(Writer& writer,
                         const Config& config,
                         const Manifest& manifest,
                         const std::set<std::string>& needed) {
    std::string text;
    text.reserve(kControlFlushThreshold);
    auto flush_text = [&]() {
        if (!text.empty()) {
            write_text(writer, text);
            text.clear();
        }
    };
    auto append_control_line = [&](std::string line) {
        text += std::move(line);
        text.push_back('\n');
        if (text.size() >= kControlFlushThreshold) {
            flush_text();
        }
    };

    SendProgress progress;
    for (const auto& path : needed) {
        const auto it = manifest.find(path);
        if (it == manifest.end() || it->second.is_directory) continue;
        ++progress.total_files;
        progress.total_bytes += it->second.size;
    }

    std::cout << "sync send start entries=" << needed.size()
              << " files=" << progress.total_files
              << " bytes=" << format_bytes(progress.total_bytes) << std::endl;

    text += "FILES " + std::to_string(needed.size()) + "\n";
    for (const auto& path : needed) {
        const auto it = manifest.find(path);
        if (it == manifest.end()) continue;
        const auto& state = it->second;
        if (state.is_directory) {
            append_control_line("DIR " + filesync::quote_token(path));
            std::cout << "sync send directory=" << filesync::quote_token(path)
                      << " total=" << format_percent(progress.total_percent()) << std::endl;
            continue;
        }
        const auto local_path = state.local_path.empty() ? local_path_for_remote(config, path) : state.local_path;
        append_control_line("PUT_BEGIN " + filesync::quote_token(path) + " " + std::to_string(state.size) + " " +
                            std::to_string(state.mtime) + " " + std::to_string(state.hash));
        flush_text();
        progress.file_throttle.reset();
        print_sync_progress("send", progress.total_percent(), path, percent_of(0, state.size), 0, state.size);
        std::ifstream in(local_path, std::ios::binary);
        if (!in) {
            throw std::runtime_error("failed to open file: " + local_path.string());
        }
        std::vector<char> chunk(config.chunk_size);
        std::uintmax_t sent = 0;
        while (in) {
            in.read(chunk.data(), static_cast<std::streamsize>(chunk.size()));
            const auto count = in.gcount();
            if (count > 0) {
                std::vector<char> bytes(chunk.begin(), chunk.begin() + count);
                write_text(writer, "CHUNK " + filesync::hex_encode(bytes) + "\n");
                sent += static_cast<std::uintmax_t>(count);
                if (progress.file_throttle.should_report(sent, state.size, false)) {
                    print_sync_progress("send", progress.total_percent(sent), path, percent_of(sent, state.size), sent, state.size);
                }
            }
        }
        append_control_line("PUT_END");
        progress.completed_bytes += state.size;
        ++progress.completed_files;
        if (sent == 0 || sent != state.size || progress.file_throttle.should_report(state.size, state.size, true)) {
            print_sync_progress("send", progress.total_percent(), path, 100.0, state.size, state.size);
        }
    }
    text += "END\n";
    flush_text();
    std::cout << "sync send complete entries=" << needed.size()
              << " files=" << progress.completed_files
              << " bytes=" << format_bytes(progress.completed_bytes) << std::endl;
}

void write_file_lines(yuan::net::ConnectionContext& ctx,
                      const Config& config,
                      const Manifest& manifest,
                      const std::set<std::string>& needed) {
    write_file_lines_to(ctx, config, manifest, needed);
}

void write_file_lines(yuan::net::StreamClientSession& session,
                        const Config& config,
                        const Manifest& manifest,
                        const std::set<std::string>& needed) {
    write_file_lines_to(session, config, manifest, needed);
}

struct FileApplyState {
    bool active = false;
    std::string path;
    FileState remote_state;
    std::filesystem::path target;
    std::filesystem::path temp;
    bool conflict = false;
    std::ofstream out;
    std::size_t total_entries = 0;
    std::size_t completed_entries = 0;
    std::uintmax_t bytes_received = 0;
    ProgressThrottle file_throttle;
};

void reset_active_put(FileApplyState& state) {
    state.active = false;
    state.path.clear();
    state.remote_state = FileState{};
    state.target.clear();
    state.temp.clear();
    state.conflict = false;
    state.out = std::ofstream{};
    state.bytes_received = 0;
    state.file_throttle.reset();
}

double receive_total_percent(const FileApplyState& state, std::uintmax_t current_file_done = 0) {
    if (state.total_entries == 0) {
        return 100.0;
    }
    double done = static_cast<double>(state.completed_entries);
    if (state.active && state.remote_state.size > 0) {
        done += static_cast<double>(current_file_done) / static_cast<double>(state.remote_state.size);
    } else if (state.active && state.remote_state.size == 0) {
        done += 1.0;
    }
    return percent_of(done, static_cast<double>(state.total_entries));
}

void print_receive_progress(FileApplyState& state, bool force) {
    if (!state.active) {
        return;
    }
    if (!state.file_throttle.should_report(state.bytes_received, state.remote_state.size, force)) {
        return;
    }
    print_sync_progress("receive",
                        receive_total_percent(state, state.bytes_received),
                        state.path,
                        percent_of(state.bytes_received, state.remote_state.size),
                        state.bytes_received,
                        state.remote_state.size);
}

void finish_put(const Config& config, FileApplyState& state) {
    (void)config;
    state.out.close();

    const bool size_matches = std::filesystem::exists(state.temp) &&
        std::filesystem::file_size(state.temp) == state.remote_state.size;
    const bool hash_matches = size_matches && filesync::fnv1a_file_hash(state.temp) == state.remote_state.hash;
    if (!hash_matches) {
        std::error_code ec;
        std::filesystem::remove(state.temp, ec);
        const auto path = state.path;
        reset_active_put(state);
        throw std::runtime_error("hash mismatch after receiving " + path);
    }

    std::filesystem::rename(state.temp, state.target);
    state.bytes_received = state.remote_state.size;
    print_receive_progress(state, true);
    std::cout << (state.conflict ? "saved conflict " : "applied ") << state.path;
    if (state.conflict) {
        std::cout << " -> " << state.target.string();
    }
    std::cout << "\n";
    ++state.completed_entries;
    reset_active_put(state);
}

bool apply_files_line(const Config& config,
                      const Manifest& last_manifest,
                      bool have_last_manifest,
                      FileApplyState& state,
                      const std::string& line) {
    if (line == "END") {
        if (state.active) {
            throw std::runtime_error("unterminated PUT");
        }
        return true;
    }
    const auto parts = filesync::split_line(line);
    if (parts.empty()) {
        return false;
    }
    if (parts[0] == "FILES") {
        if (parts.size() >= 2) {
            state.total_entries = static_cast<std::size_t>(std::stoull(parts[1]));
            state.completed_entries = 0;
            std::cout << "sync receive start entries=" << state.total_entries << std::endl;
        }
        return false;
    }
    if (state.active) {
        if (parts.size() == 2 && parts[0] == "CHUNK") {
            const auto bytes = filesync::hex_decode(parts[1]);
            state.out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
            state.bytes_received += bytes.size();
            print_receive_progress(state, false);
            return false;
        }
        if (parts.size() == 1 && parts[0] == "PUT_END") {
            finish_put(config, state);
            return false;
        }
        throw std::runtime_error("bad chunk stream");
    }
    if (parts.size() == 2 && parts[0] == "DIR") {
        const auto path = filesync::unquote_token(parts[1]);
        if (filesync::is_safe_relative_path(path) && should_include_remote_path(config, path, true)) {
            std::filesystem::create_directories(local_path_for_remote(config, path));
            ++state.completed_entries;
            std::cout << "sync receive directory=" << filesync::quote_token(path)
                      << " total=" << format_percent(percent_of(static_cast<double>(state.completed_entries),
                                                                 static_cast<double>(state.total_entries))) << std::endl;
        }
        return false;
    }
    if (parts.size() == 6 && parts[0] == "PUT") {
        const auto path = filesync::unquote_token(parts[1]);
        if (!filesync::is_safe_relative_path(path)) return false;
        if (!should_include_remote_path(config, path, false)) return false;
        const FileState remote_state{false,
                                     static_cast<std::uintmax_t>(std::stoull(parts[2])),
                                     static_cast<std::uint64_t>(std::stoull(parts[3])),
                                     static_cast<std::uint64_t>(std::stoull(parts[4]))};
        auto target = local_path_for_remote(config, path);
        const bool conflict = is_conflict(config, last_manifest, have_last_manifest, path, remote_state, target);
        if (conflict) target = conflict_path_for(target);
        std::filesystem::create_directories(target.parent_path());
        const auto bytes = filesync::hex_decode(parts[5]);
        const auto temp = target.string() + ".filesync.tmp";
        {
            std::ofstream out(temp, std::ios::binary | std::ios::trunc);
            out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        }
        const bool size_matches = std::filesystem::file_size(temp) == remote_state.size;
        const bool hash_matches = size_matches && filesync::fnv1a_file_hash(temp) == remote_state.hash;
        if (!hash_matches) {
            std::error_code ec;
            std::filesystem::remove(temp, ec);
            throw std::runtime_error("hash mismatch after receiving " + path);
        }
        std::filesystem::rename(temp, target);
        ++state.completed_entries;
        print_sync_progress("receive",
                            percent_of(static_cast<double>(state.completed_entries), static_cast<double>(state.total_entries)),
                            path,
                            100.0,
                            remote_state.size,
                            remote_state.size);
        return false;
    }
    if (parts.size() != 5 || parts[0] != "PUT_BEGIN") {
        return false;
    }
    const auto path = filesync::unquote_token(parts[1]);
    if (!filesync::is_safe_relative_path(path)) return false;
    if (!should_include_remote_path(config, path, false)) return false;
    FileState remote_state{false,
                           static_cast<std::uintmax_t>(std::stoull(parts[2])),
                           static_cast<std::uint64_t>(std::stoull(parts[3])),
                           static_cast<std::uint64_t>(std::stoull(parts[4]))};
    auto target = local_path_for_remote(config, path);
    const bool conflict = is_conflict(config, last_manifest, have_last_manifest, path, remote_state, target);
    if (conflict) target = conflict_path_for(target);
    std::filesystem::create_directories(target.parent_path());
    state.active = true;
    state.path = path;
    state.remote_state = remote_state;
    state.target = target;
    state.temp = target.string() + ".filesync.tmp";
    state.conflict = conflict;
    state.bytes_received = 0;
    state.file_throttle.reset();
    state.out.open(state.temp, std::ios::binary | std::ios::trunc);
    if (!state.out) {
        throw std::runtime_error("failed to open temp file: " + state.temp.string());
    }
    print_receive_progress(state, true);
    return false;
}

void apply_files(const Config& config, const Manifest& last_manifest, bool have_last_manifest, const std::string& text) {
    FileApplyState state;
    for (const auto& line : split_lines(text)) {
        if (apply_files_line(config, last_manifest, have_last_manifest, state, line)) {
            break;
        }
    }
}

class PeerService {
public:
    explicit PeerService(Config config) : config_(std::move(config)) {}

    bool start() {
        server_.set_connected_callback([](yuan::net::ConnectionContext& ctx) {
            ctx.set_max_packet_size(100 * 1024 * 1024);
        });
        server_.set_read_callback([this](yuan::net::ConnectionContext& ctx) { on_read(ctx); });
        server_.set_close_callback([this](yuan::net::ConnectionContext& ctx) {
            inbound_.erase(ctx.connection_id());
        });
        server_.set_error_callback([this](yuan::net::ConnectionContext& ctx) {
            inbound_.erase(ctx.connection_id());
        });
        if (!server_.bind(config_.listen_host, config_.listen_port, runtime_)) {
            return false;
        }
        if (!config_.peer_host.empty()) {
            periodic_sync_timer_ = runtime_.schedule_periodic(200, static_cast<uint32_t>(config_.scan_interval_ms), [this]() {
                start_outbound_sync();
            }, -1);
        }
        std::cout << "filesync peer listening on " << config_.listen_host << ":" << config_.listen_port << "\n";
        std::cout << "  peer_host=" << (config_.peer_host.empty() ? "<inbound-only>" : config_.peer_host + ":" + std::to_string(config_.peer_port))
                  << " sync_deletes=" << (config_.sync_deletes ? "on" : "off")
                  << " conflict_strategy=" << config_.conflict_strategy
                  << " scan_interval_ms=" << config_.scan_interval_ms
                  << " chunk_size=" << config_.chunk_size << "\n";
        for (const auto& sync_path : config_.paths) {
            std::cout << "  path " << sync_path.local.string();
            if (!sync_path.remote_prefix.empty()) {
                std::cout << " -> " << sync_path.remote_prefix;
            }
            std::cout << "\n";
        }
        runtime_.run();
        return true;
    }

private:
    enum class InState { WaitingManifest, WaitingFiles, WaitingNeedForUpload, WaitingAck };

    struct Inbound {
        InState state = InState::WaitingManifest;
        std::string buffer;
        Manifest upload_manifest;
        bool have_upload_manifest = false;
        FileApplyState file_apply_state;
    };

    void on_read(yuan::net::ConnectionContext& ctx) {
        auto& state = inbound_[ctx.connection_id()];
        state.buffer += read_buffer_text(ctx);
        while (true) {
            std::string frame;
            if (state.state == InState::WaitingFiles) {
                const auto pos = state.buffer.find('\n');
                if (pos == std::string::npos) {
                    break;
                }
                frame = state.buffer.substr(0, pos);
                if (!frame.empty() && frame.back() == '\r') frame.pop_back();
                state.buffer.erase(0, pos + 1);
            } else if (!extract_next_frame(state.buffer, frame)) {
                break;
            }
            try {
                if (state.state == InState::WaitingManifest) {
                    const auto remote = parse_manifest(split_lines(frame), config_);
                    write_text(ctx, need_message(config_, remote));
                    state.state = InState::WaitingFiles;
                } else if (state.state == InState::WaitingFiles) {
                    if (apply_files_line(config_, last_manifest_, have_last_manifest_, state.file_apply_state, frame)) {
                        state.upload_manifest = scan_paths(config_);
                        state.have_upload_manifest = true;
                        save_state(config_, state.upload_manifest);
                        write_manifest(ctx, config_, state.upload_manifest);
                        state.state = InState::WaitingNeedForUpload;
                    }
                } else if (state.state == InState::WaitingNeedForUpload) {
                    if (!state.have_upload_manifest) {
                        throw std::runtime_error("upload manifest not prepared");
                    }
                    apply_need_deletes(config_, frame);
                    const auto needed = parse_need(frame);
                    write_file_lines(ctx, config_, state.upload_manifest, needed);
                    state.state = InState::WaitingAck;
                } else {
                    const auto lines = split_lines(frame);
                    if (lines.empty() || filesync::split_line(lines.front()).empty() ||
                        filesync::split_line(lines.front()).front() != "OK") {
                        throw std::runtime_error("bad sync ack");
                    }
                    state.state = InState::WaitingManifest;
                    state.have_upload_manifest = false;
                    state.upload_manifest.clear();
                    write_text(ctx, "READY\nEND\n");
                }
            } catch (const std::exception& ex) {
                std::cerr << "inbound sync error: " << ex.what() << "\n";
                ctx.close();
                inbound_.erase(ctx.connection_id());
                break;
            }
        }
    }

    yuan::coroutine::Task<std::string> read_frame(yuan::net::StreamClientSession& session, std::string& buffer) {
        std::string frame;
        while (!extract_next_frame(buffer, frame)) {
            auto packet = co_await session.read_async();
            const auto span = packet.readable_span();
            if (span.empty()) {
                throw std::runtime_error("peer closed while waiting frame");
            }
            buffer.append(span.begin(), span.end());
        }
        co_return frame;
    }

    yuan::coroutine::Task<std::string> read_line(yuan::net::StreamClientSession& session, std::string& buffer) {
        while (true) {
            const auto pos = buffer.find('\n');
            if (pos != std::string::npos) {
                auto line = buffer.substr(0, pos);
                if (!line.empty() && line.back() == '\r') line.pop_back();
                buffer.erase(0, pos + 1);
                co_return line;
            }
            auto packet = co_await session.read_async();
            const auto span = packet.readable_span();
            if (span.empty()) {
                throw std::runtime_error("peer closed while waiting line");
            }
            buffer.append(span.begin(), span.end());
        }
    }

    yuan::coroutine::Task<void> read_files(yuan::net::StreamClientSession& session, std::string& buffer) {
        FileApplyState state;
        while (true) {
            const auto line = co_await read_line(session, buffer);
            if (apply_files_line(config_, last_manifest_, have_last_manifest_, state, line)) {
                co_return;
            }
        }
    }

    yuan::coroutine::Task<void> outbound_sync() {
        if (syncing_) co_return;
        const auto manifest = scan_paths(config_);
        syncing_ = true;
        try {
            if (!outbound_session_) {
                outbound_session_ = std::make_shared<yuan::net::StreamClientSession>();
            }
            if (!outbound_session_->is_connected()) {
                bool connected = co_await outbound_session_->connect_async(runtime_.runtime_view().raw(), config_.peer_host, config_.peer_port, 0);
                if (!connected) {
                    last_sync_ok_ = false;
                    syncing_ = false;
                    co_return;
                }
                outbound_session_->context().set_max_packet_size(100 * 1024 * 1024);
            }

            if (!outbound_session_->is_connected()) {
                last_sync_ok_ = false;
                syncing_ = false;
                co_return;
            }

            std::string receive_buffer;

            write_manifest(*outbound_session_, config_, manifest);

            const auto need_text = co_await read_frame(*outbound_session_, receive_buffer);
            apply_need_deletes(config_, need_text);
            const auto needed = parse_need(need_text);
            write_file_lines(*outbound_session_, config_, manifest, needed);

            const auto reverse_manifest_text = co_await read_frame(*outbound_session_, receive_buffer);
            const auto reverse_manifest = parse_manifest(split_lines(reverse_manifest_text), config_);
            const auto reverse_need_text = need_message(config_, reverse_manifest);
            yuan::buffer::ByteBuffer reverse_need_buffer{std::string_view(reverse_need_text)};
            outbound_session_->write_and_flush(reverse_need_buffer);

            co_await read_files(*outbound_session_, receive_buffer);

            yuan::buffer::ByteBuffer ack{std::string_view("OK\nEND\n")};
            outbound_session_->write_and_flush(ack);
            const auto ready_text = co_await read_frame(*outbound_session_, receive_buffer);
            if (!is_ready_frame(ready_text)) {
                throw std::runtime_error("peer did not finish sync round");
            }

            last_manifest_ = scan_paths(config_);
            have_last_manifest_ = true;
            save_state(config_, last_manifest_);
            last_sync_ok_ = true;
            syncing_ = false;
        } catch (const std::exception& ex) {
            std::cerr << "outbound sync error: " << ex.what() << "\n";
            if (outbound_session_) {
                outbound_session_->close();
            }
            last_sync_ok_ = false;
            syncing_ = false;
        }
        co_return;
    }

    void start_outbound_sync() {
        try {
            if (outbound_task_) {
                if (!outbound_task_->done()) {
                    return;
                }
                outbound_task_->get_result();
                outbound_task_.reset();
            }
            outbound_task_ = std::make_unique<yuan::coroutine::Task<void>>(outbound_sync());
            outbound_task_->resume();
            if (outbound_task_ && outbound_task_->done()) {
                outbound_task_->get_result();
                outbound_task_.reset();
            }
        } catch (const std::exception& ex) {
            std::cerr << "outbound tick error: " << ex.what() << "\n";
            outbound_task_.reset();
            syncing_ = false;
            last_sync_ok_ = false;
        }
    }

    Config config_;
    yuan::net::NetworkRuntime runtime_;
    yuan::net::StreamServerSession server_;
    std::unordered_map<uintptr_t, Inbound> inbound_;
    bool syncing_ = false;
    bool have_last_manifest_ = false;
    bool last_sync_ok_ = false;
    Manifest last_manifest_;
    std::unique_ptr<yuan::coroutine::Task<void>> outbound_task_;
    yuan::timer::TimerHandle periodic_sync_timer_;
    std::shared_ptr<yuan::net::StreamClientSession> outbound_session_;
};

} // namespace

class ReleaseFileSyncPeerApp {
public:
    int start(int argc, char** argv) {
        #ifdef FILESYNC_DEFAULT_CONFIG
        const std::filesystem::path default_config = FILESYNC_DEFAULT_CONFIG;
        #else
        const std::filesystem::path default_config = "peer_config.json";
        #endif
        const std::filesystem::path config_path = argc > 1 ? argv[1] : default_config;
        try {
            PeerService service(load_config(config_path));
            return service.start() ? 0 : 1;
        } catch (const std::exception& ex) {
            std::cerr << "filesync error: " << ex.what() << "\n";
            return 1;
        }
    }
};

#ifndef FILESYNC_PEER_TESTING
int main(int argc, char** argv) {
    ReleaseFileSyncPeerApp app;
    return app.start(argc, argv);
}
#endif
