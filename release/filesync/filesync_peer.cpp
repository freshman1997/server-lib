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

    bool operator==(const FileState& other) const {
        return is_directory == other.is_directory && size == other.size && mtime == other.mtime && hash == other.hash;
    }
};

using Manifest = std::map<std::string, FileState>;

std::vector<std::string> json_string_array(const nlohmann::json& json, const char* key) {
    std::vector<std::string> values;
    if (!json.contains(key)) {
        return values;
    }
    for (const auto& item : json.at(key)) {
        values.push_back(item.get<std::string>());
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
            config.paths.push_back({item.at("local").get<std::string>(), item.value("remote_prefix", "")});
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
    return filesync::normalize_relative_path(std::filesystem::path(sync_path.remote_prefix) / rel);
}

std::filesystem::path local_path_for_remote(const Config& config, const std::string& remote) {
    for (const auto& sync_path : config.paths) {
        const auto prefix = filesync::normalize_relative_path(sync_path.remote_prefix);
        if (std::filesystem::is_regular_file(sync_path.local)) {
            const auto mapped = prefix.empty() ? sync_path.local.filename().generic_string() : prefix;
            if (remote == mapped) return sync_path.local;
            continue;
        }
        if (prefix.empty()) {
            return sync_path.local / remote;
        }
        if (remote == prefix) {
            return sync_path.local;
        }
        const auto marker = prefix + "/";
        if (remote.rfind(marker, 0) == 0) {
            return sync_path.local / remote.substr(marker.size());
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

std::string glob_to_regex(const std::string& glob) {
    std::string out = "^";
    for (std::size_t i = 0; i < glob.size(); ++i) {
        const char ch = glob[i];
        if (ch == '*') {
            if (i + 1 < glob.size() && glob[i + 1] == '*') {
                out += ".*";
                ++i;
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

bool any_pattern_matches(const std::vector<std::string>& patterns, const std::string& remote) {
    return std::any_of(patterns.begin(), patterns.end(), [&](const std::string& pattern) {
        return glob_match(pattern, remote);
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

bool should_include_file(const Config& config, const std::filesystem::path& local, const std::string& remote) {
    if (is_internal_file(local)) {
        return false;
    }
    if (!extension_allowed(config, local)) {
        return false;
    }
    if (!config.include_patterns.empty() && !any_pattern_matches(config.include_patterns, remote)) {
        return false;
    }
    if (any_pattern_matches(config.exclude_patterns, remote)) {
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
            if (!should_include_file(config, sync_path.local, remote)) {
                continue;
            }
            manifest[remote] = {
                false,
                std::filesystem::file_size(sync_path.local),
                filesync::file_time_to_seconds(std::filesystem::last_write_time(sync_path.local)),
                filesync::fnv1a_file_hash(sync_path.local),
            };
            continue;
        }
        for (const auto& entry : std::filesystem::recursive_directory_iterator(sync_path.local)) {
            const auto relative = std::filesystem::relative(entry.path(), sync_path.local);
            const auto remote = make_remote_path(sync_path, relative);
            if (entry.is_directory()) {
                manifest[remote] = {true, 0, 0, 0};
            } else if (entry.is_regular_file()) {
                if (!should_include_file(config, entry.path(), remote)) {
                    continue;
                }
                manifest[remote] = {
                    false,
                    entry.file_size(),
                    filesync::file_time_to_seconds(entry.last_write_time()),
                    filesync::fnv1a_file_hash(entry.path()),
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
    nlohmann::json json;
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
    std::ifstream in(state_file_path(config));
    if (!in) {
        return manifest;
    }
    nlohmann::json json;
    in >> json;
    for (const auto& item : json.value("manifest", nlohmann::json::array())) {
        manifest[item.at("path").get<std::string>()] = {
            item.value("is_directory", false),
            static_cast<std::uintmax_t>(item.value("size", 0ull)),
            static_cast<std::uint64_t>(item.value("mtime", 0ull)),
            static_cast<std::uint64_t>(item.value("hash", 0ull)),
        };
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
        if (deleted_set.find(path) != deleted_set.end()) {
            continue;
        }
        if (should_pull(local, path, state)) {
            needed.push_back(path);
        }
    }
    std::ostringstream out;
    out << "NEED " << needed.size() << "\n";
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
    std::error_code ec;
    if (std::filesystem::is_directory(target, ec)) {
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
            needed.insert(filesync::unquote_token(parts[1]));
        } else if (parts.size() == 1) {
            needed.insert(filesync::unquote_token(parts[0]));
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

void write_file_lines(yuan::net::ConnectionContext& ctx,
                      const Config& config,
                      const Manifest& manifest,
                      const std::set<std::string>& needed) {
    std::string text;
    text.reserve(64 * 1024);
    auto flush_text = [&]() {
        if (!text.empty()) {
            write_text(ctx, text);
            text.clear();
        }
    };
    text += "FILES " + std::to_string(needed.size()) + "\n";
    for (const auto& path : needed) {
        const auto it = manifest.find(path);
        if (it == manifest.end()) continue;
        const auto& state = it->second;
        if (state.is_directory) {
            text += "DIR " + filesync::quote_token(path) + "\n";
            continue;
        }
        const auto local_path = local_path_for_remote(config, path);
        text += "PUT_BEGIN " + filesync::quote_token(path) + " " + std::to_string(state.size) + " " +
                std::to_string(state.mtime) + " " + std::to_string(state.hash) + "\n";
        flush_text();
        std::ifstream in(local_path, std::ios::binary);
        if (!in) {
            throw std::runtime_error("failed to open file: " + local_path.string());
        }
        std::vector<char> chunk(config.chunk_size);
        while (in) {
            in.read(chunk.data(), static_cast<std::streamsize>(chunk.size()));
            const auto count = in.gcount();
            if (count > 0) {
                std::vector<char> bytes(chunk.begin(), chunk.begin() + count);
                write_text(ctx, "CHUNK " + filesync::hex_encode(bytes) + "\n");
            }
        }
        text += "PUT_END\n";
    }
    text += "END\n";
    flush_text();
}

void write_file_lines(yuan::net::StreamClientSession& session,
                        const Config& config,
                        const Manifest& manifest,
                        const std::set<std::string>& needed) {
    std::string text;
    text.reserve(64 * 1024);
    auto flush_text = [&]() {
        if (!text.empty()) {
            write_text(session, text);
            text.clear();
        }
    };
    text += "FILES " + std::to_string(needed.size()) + "\n";
    for (const auto& path : needed) {
        const auto it = manifest.find(path);
        if (it == manifest.end()) continue;
        const auto& state = it->second;
        if (state.is_directory) {
            text += "DIR " + filesync::quote_token(path) + "\n";
            continue;
        }
        const auto local_path = local_path_for_remote(config, path);
        text += "PUT_BEGIN " + filesync::quote_token(path) + " " + std::to_string(state.size) + " " +
                std::to_string(state.mtime) + " " + std::to_string(state.hash) + "\n";
        flush_text();
        std::ifstream in(local_path, std::ios::binary);
        if (!in) {
            throw std::runtime_error("failed to open file: " + local_path.string());
        }
        std::vector<char> chunk(config.chunk_size);
        while (in) {
            in.read(chunk.data(), static_cast<std::streamsize>(chunk.size()));
            const auto count = in.gcount();
            if (count > 0) {
                std::vector<char> bytes(chunk.begin(), chunk.begin() + count);
                write_text(session, "CHUNK " + filesync::hex_encode(bytes) + "\n");
            }
        }
        text += "PUT_END\n";
    }
    text += "END\n";
    flush_text();
}

struct FileApplyState {
    bool active = false;
    std::string path;
    FileState remote_state;
    std::filesystem::path target;
    std::filesystem::path temp;
    bool conflict = false;
    std::ofstream out;
};

void finish_put(const Config& config, FileApplyState& state) {
    state.out.close();
    if (state.conflict) {
        std::filesystem::create_directories(state.target.parent_path());
    }
    std::filesystem::rename(state.temp, state.target);
    if (filesync::fnv1a_file_hash(state.target) == state.remote_state.hash) {
        std::cout << (state.conflict ? "saved conflict " : "applied ") << state.path;
        if (state.conflict) {
            std::cout << " -> " << state.target.string();
        }
        std::cout << "\n";
    } else {
        std::cerr << "hash mismatch after applying " << state.path << "\n";
    }
    state = FileApplyState{};
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
    if (parts.empty() || parts[0] == "FILES") {
        return false;
    }
    if (state.active) {
        if (parts.size() == 2 && parts[0] == "CHUNK") {
            const auto bytes = filesync::hex_decode(parts[1]);
            state.out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
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
        if (filesync::is_safe_relative_path(path)) {
            std::filesystem::create_directories(local_path_for_remote(config, path));
        }
        return false;
    }
    if (parts.size() == 6 && parts[0] == "PUT") {
        const auto path = filesync::unquote_token(parts[1]);
        if (!filesync::is_safe_relative_path(path)) return false;
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
        std::filesystem::rename(temp, target);
        if (filesync::fnv1a_file_hash(target) != remote_state.hash) {
            std::cerr << "hash mismatch after applying " << path << "\n";
        }
        return false;
    }
    if (parts.size() != 5 || parts[0] != "PUT_BEGIN") {
        return false;
    }
    const auto path = filesync::unquote_token(parts[1]);
    if (!filesync::is_safe_relative_path(path)) return false;
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
    state.out.open(state.temp, std::ios::binary | std::ios::trunc);
    if (!state.out) {
        throw std::runtime_error("failed to open temp file: " + state.temp.string());
    }
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
                        write_text(ctx, manifest_message(config_, state.upload_manifest));
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
                    state.state = InState::WaitingManifest;
                    state.have_upload_manifest = false;
                    state.upload_manifest.clear();
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
        if (last_sync_ok_ && have_last_manifest_ && manifest == last_manifest_) {
            co_return;
        }
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

            const auto manifest_text = manifest_message(config_, manifest);
            yuan::buffer::ByteBuffer manifest_buffer{std::string_view(manifest_text)};
            outbound_session_->write_and_flush(manifest_buffer);

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
                    outbound_task_->resume();
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

int main(int argc, char** argv) {
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
