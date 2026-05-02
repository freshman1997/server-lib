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
    int scan_interval_ms = 1000;
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
    config.scan_interval_ms = json.value("scan_interval_ms", config.scan_interval_ms);
    config.include_extensions = json_string_array(json, "include_extensions");
    config.include_patterns = json_string_array(json, "include_patterns");
    config.exclude_patterns = json_string_array(json, "exclude_patterns");
    for (const auto& item : json.at("paths")) {
        config.paths.push_back({item.at("local").get<std::string>(), item.value("remote_prefix", "")});
    }
    if (config.paths.empty()) {
        throw std::runtime_error("config paths is empty");
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
    return name.find(".conflict.") != std::string::npos ||
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
                manifest[remote] = {true, 0, filesync::file_time_to_seconds(entry.last_write_time()), 0};
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

std::string need_message(const Config& config, const Manifest& remote) {
    const auto local = scan_paths(config);
    std::vector<std::string> needed;
    for (const auto& [path, state] : remote) {
        if (should_pull(local, path, state)) {
            needed.push_back(path);
        }
    }
    std::ostringstream out;
    out << "NEED " << needed.size() << "\n";
    for (const auto& path : needed) {
        out << filesync::quote_token(path) << "\n";
    }
    out << "END\n";
    return out.str();
}

std::set<std::string> parse_need(const std::string& text) {
    const auto lines = split_lines(text);
    std::set<std::string> needed;
    for (std::size_t i = 1; i + 1 < lines.size(); ++i) {
        needed.insert(filesync::unquote_token(lines[i]));
    }
    return needed;
}

std::string files_message(const Config& config, const Manifest& manifest, const std::set<std::string>& needed) {
    std::ostringstream out;
    out << "FILES " << needed.size() << "\n";
    for (const auto& path : needed) {
        const auto it = manifest.find(path);
        if (it == manifest.end()) continue;
        const auto& state = it->second;
        if (state.is_directory) {
            out << "DIR " << filesync::quote_token(path) << "\n";
            continue;
        }
        const auto local_path = local_path_for_remote(config, path);
        const auto bytes = filesync::read_file_bytes(local_path);
        out << "PUT " << filesync::quote_token(path) << " " << state.size << " " << state.mtime << " "
            << state.hash << " " << filesync::hex_encode(bytes) << "\n";
    }
    out << "END\n";
    return out.str();
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

void apply_files(const Config& config, const Manifest& last_manifest, bool have_last_manifest, const std::string& text) {
    const auto lines = split_lines(text);
    for (std::size_t i = 1; i + 1 < lines.size(); ++i) {
        const auto parts = filesync::split_line(lines[i]);
        if (parts.size() == 2 && parts[0] == "DIR") {
            const auto path = filesync::unquote_token(parts[1]);
            if (filesync::is_safe_relative_path(path)) {
                std::filesystem::create_directories(local_path_for_remote(config, path));
            }
            continue;
        }
        if (parts.size() != 6 || parts[0] != "PUT") continue;
        const auto path = filesync::unquote_token(parts[1]);
        if (!filesync::is_safe_relative_path(path)) continue;
        const FileState remote_state{
            false,
            static_cast<std::uintmax_t>(std::stoull(parts[2])),
            static_cast<std::uint64_t>(std::stoull(parts[3])),
            static_cast<std::uint64_t>(std::stoull(parts[4])),
        };
        const auto expected_hash = remote_state.hash;
        const auto bytes = filesync::hex_decode(parts[5]);
        auto target = local_path_for_remote(config, path);
        const bool conflict = is_conflict(config, last_manifest, have_last_manifest, path, remote_state, target);
        if (conflict) {
            target = conflict_path_for(target);
        }
        std::filesystem::create_directories(target.parent_path());
        const auto temp = target.string() + ".filesync.tmp";
        {
            std::ofstream out(temp, std::ios::binary | std::ios::trunc);
            out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        }
        std::filesystem::rename(temp, target);
        if (filesync::fnv1a_file_hash(target) == expected_hash) {
            std::cout << (conflict ? "saved conflict " : "applied ") << path;
            if (conflict) {
                std::cout << " -> " << target.string();
            }
            std::cout << "\n";
        } else {
            std::cerr << "hash mismatch after applying " << path << "\n";
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
            runtime_.schedule_periodic(200, static_cast<uint32_t>(config_.scan_interval_ms), [this]() {
                start_outbound_sync();
            }, -1);
        }
        std::cout << "filesync peer listening on " << config_.listen_host << ":" << config_.listen_port << "\n";
        runtime_.run();
        return true;
    }

private:
    enum class InState { WaitingManifest, WaitingFiles };

    struct Inbound {
        InState state = InState::WaitingManifest;
        std::string buffer;
    };

    void on_read(yuan::net::ConnectionContext& ctx) {
        auto& state = inbound_[ctx.connection_id()];
        state.buffer += read_buffer_text(ctx);
        if (state.buffer.find("\nEND\n") == std::string::npos) {
            return;
        }
        try {
            if (state.state == InState::WaitingManifest) {
                const auto remote = parse_manifest(split_lines(state.buffer), config_);
                write_text(ctx, need_message(config_, remote));
                state.buffer.clear();
                state.state = InState::WaitingFiles;
            } else {
                apply_files(config_, last_manifest_, have_last_manifest_, state.buffer);
                write_text(ctx, "OK\nEND\n");
                ctx.close();
                inbound_.erase(ctx.connection_id());
            }
        } catch (const std::exception& ex) {
            std::cerr << "inbound sync error: " << ex.what() << "\n";
            ctx.close();
            inbound_.erase(ctx.connection_id());
        }
    }

    yuan::coroutine::Task<void> outbound_sync() {
        if (syncing_) co_return;
        const auto manifest = scan_paths(config_);
        if (last_sync_ok_ && have_last_manifest_ && manifest == last_manifest_) {
            co_return;
        }
        syncing_ = true;
        auto session = std::make_shared<yuan::net::StreamClientSession>();
        bool connected = co_await session->connect_async(runtime_.runtime_view().raw(), config_.peer_host, config_.peer_port, 3000);
        if (!connected) {
            last_sync_ok_ = false;
            syncing_ = false;
            co_return;
        }
        session->context().set_max_packet_size(100 * 1024 * 1024);
        const auto manifest_text = manifest_message(config_, manifest);
        yuan::buffer::ByteBuffer manifest_buffer{std::string_view(manifest_text)};
        session->write_and_flush(manifest_buffer);
        auto need_buffer = co_await session->read_async();
        const auto need_span = need_buffer.readable_span();
        const auto need_text = std::string(need_span.begin(), need_span.end());
        const auto needed = parse_need(need_text);
        const auto files_text = files_message(config_, manifest, needed);
        yuan::buffer::ByteBuffer files_buffer{std::string_view(files_text)};
        session->write_and_flush(files_buffer);
        session->close();
        last_manifest_ = manifest;
        have_last_manifest_ = true;
        last_sync_ok_ = true;
        syncing_ = false;
        co_return;
    }

    void start_outbound_sync() {
        auto task = outbound_sync();
        task.resume();
        task.detach();
    }

    Config config_;
    yuan::net::NetworkRuntime runtime_;
    yuan::net::StreamServerSession server_;
    std::unordered_map<uintptr_t, Inbound> inbound_;
    bool syncing_ = false;
    bool have_last_manifest_ = false;
    bool last_sync_ok_ = false;
    Manifest last_manifest_;
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
