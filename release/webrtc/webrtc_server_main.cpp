#include "webrtc.h"

#include "nlohmann/json.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
using SocketHandle = SOCKET;
constexpr SocketHandle kInvalidSocket = INVALID_SOCKET;
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
using SocketHandle = int;
constexpr SocketHandle kInvalidSocket = -1;
#endif

namespace
{
using Json = nlohmann::json;
using namespace yuan::net::webrtc;

struct ServerConfig
{
    std::string host = "0.0.0.0";
    int port = 9000;
    std::string probe_host = "127.0.0.1";
    uint32_t local_ssrc = 0x10203040u;
    uint32_t clock_rate = 90000;
    bool diagnostics_keep_latest_only = true;
    std::size_t nomination_max_pending_signals = 16;
    std::size_t diagnostics_max_pending_signals = 8;
    bool diagnostics_emit_flat_compat_fields = false;
    bool diagnostics_release_mode_strict_v2 = false;
    uint64_t diagnostics_rollout_alert_window_seconds = 86400;
    uint64_t diagnostics_rollout_mismatch_count_alert_threshold = 0;
    uint64_t diagnostics_rollout_mismatch_ratio_threshold_ppm = 1000;
    int health_log_interval_ms = 5000;
};

std::atomic<bool> g_running{ true };

void signal_handler(int)
{
    g_running.store(false);
}

void close_socket(SocketHandle fd)
{
    if (fd == kInvalidSocket) {
        return;
    }
#ifdef _WIN32
    closesocket(fd);
#else
    close(fd);
#endif
}

uint64_t now_ms()
{
    using namespace std::chrono;
    return static_cast<uint64_t>(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

std::string read_env_string(const char *name, const std::string &default_value = {})
{
    const char *raw = std::getenv(name);
    return raw ? std::string(raw) : default_value;
}

int read_env_int(const char *name, int default_value)
{
    const char *raw = std::getenv(name);
    if (!raw || *raw == '\0') {
        return default_value;
    }
    try {
        return std::stoi(raw);
    } catch (...) {
        return default_value;
    }
}

std::filesystem::path default_config_path()
{
    const auto env_path = read_env_string("YUAN_WEBRTC_CONFIG", "");
    if (!env_path.empty()) {
        return std::filesystem::path(env_path);
    }
    if (std::filesystem::exists(std::filesystem::path("release/webrtc/config.json"))) {
        return std::filesystem::path("release/webrtc/config.json");
    }
    return std::filesystem::path("config.json");
}

bool parse_int_value(const std::string &raw, int &out)
{
    try {
        std::size_t pos = 0;
        const int value = std::stoi(raw, &pos);
        if (pos != raw.size()) {
            return false;
        }
        out = value;
        return true;
    } catch (...) {
        return false;
    }
}

bool load_config_file(const std::filesystem::path &path, ServerConfig &config, std::string &error)
{
    std::ifstream in(path);
    if (!in.good()) {
        error = "cannot open config file: " + path.string();
        return false;
    }

    Json json;
    try {
        in >> json;
    } catch (const std::exception &ex) {
        error = std::string("invalid config json: ") + ex.what();
        return false;
    }

    if (json.contains("host") && json["host"].is_string()) {
        config.host = json["host"].get<std::string>();
    }
    if (json.contains("port") && json["port"].is_number_integer()) {
        config.port = json["port"].get<int>();
    }
    if (json.contains("probe_host") && json["probe_host"].is_string()) {
        config.probe_host = json["probe_host"].get<std::string>();
    }
    if (json.contains("local_ssrc") && json["local_ssrc"].is_number_unsigned()) {
        config.local_ssrc = json["local_ssrc"].get<uint32_t>();
    }
    if (json.contains("clock_rate") && json["clock_rate"].is_number_unsigned()) {
        config.clock_rate = json["clock_rate"].get<uint32_t>();
    }

    if (json.contains("diagnostics_keep_latest_only") && json["diagnostics_keep_latest_only"].is_boolean()) {
        config.diagnostics_keep_latest_only = json["diagnostics_keep_latest_only"].get<bool>();
    }
    if (json.contains("nomination_max_pending_signals") && json["nomination_max_pending_signals"].is_number_unsigned()) {
        config.nomination_max_pending_signals = json["nomination_max_pending_signals"].get<std::size_t>();
    }
    if (json.contains("diagnostics_max_pending_signals") && json["diagnostics_max_pending_signals"].is_number_unsigned()) {
        config.diagnostics_max_pending_signals = json["diagnostics_max_pending_signals"].get<std::size_t>();
    }
    if (json.contains("diagnostics_emit_flat_compat_fields") && json["diagnostics_emit_flat_compat_fields"].is_boolean()) {
        config.diagnostics_emit_flat_compat_fields = json["diagnostics_emit_flat_compat_fields"].get<bool>();
    }
    if (json.contains("diagnostics_release_mode_strict_v2") && json["diagnostics_release_mode_strict_v2"].is_boolean()) {
        config.diagnostics_release_mode_strict_v2 = json["diagnostics_release_mode_strict_v2"].get<bool>();
    }
    if (json.contains("diagnostics_rollout_alert_window_seconds") && json["diagnostics_rollout_alert_window_seconds"].is_number_unsigned()) {
        config.diagnostics_rollout_alert_window_seconds = json["diagnostics_rollout_alert_window_seconds"].get<uint64_t>();
    }
    if (json.contains("diagnostics_rollout_mismatch_count_alert_threshold")
        && json["diagnostics_rollout_mismatch_count_alert_threshold"].is_number_unsigned()) {
        config.diagnostics_rollout_mismatch_count_alert_threshold =
            json["diagnostics_rollout_mismatch_count_alert_threshold"].get<uint64_t>();
    }
    if (json.contains("diagnostics_rollout_mismatch_ratio_threshold_ppm")
        && json["diagnostics_rollout_mismatch_ratio_threshold_ppm"].is_number_unsigned()) {
        config.diagnostics_rollout_mismatch_ratio_threshold_ppm =
            json["diagnostics_rollout_mismatch_ratio_threshold_ppm"].get<uint64_t>();
    }
    if (json.contains("health_log_interval_ms") && json["health_log_interval_ms"].is_number_integer()) {
        config.health_log_interval_ms = json["health_log_interval_ms"].get<int>();
    }

    return true;
}

void print_usage(const char *program)
{
    std::cout
        << "release_webrtc_server\n"
        << "usage:\n"
        << "  " << program << " [--config <file>] [--host <host>] [--port <port>]\n"
        << "  " << program << " [--self-check-only] [--probe-host <host>]\n\n"
        << "options:\n"
        << "  -f, --config <file>      Read server config JSON\n"
        << "      --host <host>        TCP bind host for signaling JSON lines\n"
        << "  -p, --port <port>        TCP listen port\n"
        << "      --probe-host <host>  Probe host for --self-check-only\n"
        << "      --self-check-only    Probe running server and exit\n"
        << "      --version            Print version\n"
        << "  -h, --help               Show this help\n\n"
        << "env overrides:\n"
        << "  YUAN_WEBRTC_CONFIG, YUAN_WEBRTC_PORT\n";
}

std::string signaling_state_name(SignalingState state)
{
    switch (state) {
    case SignalingState::new_:
        return "new";
    case SignalingState::have_local_offer:
        return "have_local_offer";
    case SignalingState::have_remote_offer:
        return "have_remote_offer";
    case SignalingState::stable:
        return "stable";
    default:
        return "unknown";
    }
}

SocketHandle create_listen_socket(const std::string &host, int port, std::string &error)
{
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    hints.ai_flags = AI_PASSIVE;

    const std::string service = std::to_string(port);
    addrinfo *result = nullptr;
    const char *host_ptr = host.empty() ? nullptr : host.c_str();
    const int gai = getaddrinfo(host_ptr, service.c_str(), &hints, &result);
    if (gai != 0 || !result) {
        error = "getaddrinfo failed";
        return kInvalidSocket;
    }

    SocketHandle listen_fd = kInvalidSocket;
    for (addrinfo *it = result; it != nullptr; it = it->ai_next) {
        SocketHandle fd = socket(it->ai_family, it->ai_socktype, it->ai_protocol);
        if (fd == kInvalidSocket) {
            continue;
        }

        int yes = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char *>(&yes), sizeof(yes));

        if (bind(fd, it->ai_addr, static_cast<int>(it->ai_addrlen)) != 0) {
            close_socket(fd);
            continue;
        }
        if (listen(fd, 32) != 0) {
            close_socket(fd);
            continue;
        }

        listen_fd = fd;
        break;
    }

    freeaddrinfo(result);
    if (listen_fd == kInvalidSocket) {
        error = "bind/listen failed";
    }
    return listen_fd;
}

bool probe_tcp(const std::string &host, int port)
{
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    const std::string service = std::to_string(port);
    addrinfo *result = nullptr;
    const int gai = getaddrinfo(host.c_str(), service.c_str(), &hints, &result);
    if (gai != 0 || !result) {
        return false;
    }

    bool ok = false;
    for (addrinfo *it = result; it != nullptr; it = it->ai_next) {
        SocketHandle fd = socket(it->ai_family, it->ai_socktype, it->ai_protocol);
        if (fd == kInvalidSocket) {
            continue;
        }
        if (connect(fd, it->ai_addr, static_cast<int>(it->ai_addrlen)) == 0) {
            ok = true;
            close_socket(fd);
            break;
        }
        close_socket(fd);
    }

    freeaddrinfo(result);
    return ok;
}

bool send_all(SocketHandle fd, const std::string &text)
{
    const char *data = text.data();
    std::size_t total = 0;
    const std::size_t length = text.size();
    while (total < length) {
        const int sent = send(fd, data + total, static_cast<int>(length - total), 0);
        if (sent <= 0) {
            return false;
        }
        total += static_cast<std::size_t>(sent);
    }
    return true;
}

std::string line_response(WebrtcPeerSession &peer, const std::string &line)
{
    Json response;
    try {
        const Json request = Json::parse(line);
        if (!request.is_object()) {
            response["ok"] = false;
            response["error"] = "request_must_be_json_object";
            return response.dump() + "\n";
        }

        if (request.contains("cmd") && request["cmd"].is_string()) {
            const std::string cmd = request["cmd"].get<std::string>();
            if (cmd == "health") {
                response["ok"] = true;
                response["health"] = Json::parse(peer.rollout_health_json());
                return response.dump() + "\n";
            }
            if (cmd == "snapshot") {
                response["ok"] = true;
                response["snapshot"] = Json::parse(peer.snapshot_json());
                return response.dump() + "\n";
            }
            if (cmd == "runtime_config_get") {
                response["ok"] = true;
                response["runtime_config"] = Json::parse(peer.signal_queue_runtime_config_json());
                return response.dump() + "\n";
            }
            if (cmd == "runtime_config_set") {
                if (!request.contains("config") || !request["config"].is_object()) {
                    response["ok"] = false;
                    response["error"] = "config_object_required";
                    return response.dump() + "\n";
                }
                SignalQueueRuntimeConfig cfg;
                const std::string cfg_json = request["config"].dump();
                if (!peer.parse_signal_queue_runtime_config_json(cfg_json, cfg)) {
                    response["ok"] = false;
                    response["error"] = "invalid_runtime_config";
                    return response.dump() + "\n";
                }
                peer.set_signal_queue_runtime_config(cfg);
                response["ok"] = true;
                response["runtime_config"] = Json::parse(peer.signal_queue_runtime_config_json());
                return response.dump() + "\n";
            }

            response["ok"] = false;
            response["error"] = "unknown_cmd";
            return response.dump() + "\n";
        }

        const bool as_remote = !request.contains("as_remote") || request["as_remote"].get<bool>();
        if (!peer.apply_signaling_json(request.dump(), as_remote, now_ms())) {
            response["ok"] = false;
            response["error"] = "apply_signaling_failed";
            response["last_sdp_error"] = peer.signaling_bridge().last_sdp_error();
            return response.dump() + "\n";
        }

        response["ok"] = true;
        response["signaling_state"] = signaling_state_name(peer.signaling_state());
        response["media_ready"] = peer.is_media_ready();
        response["health"] = Json::parse(peer.rollout_health_json());
        return response.dump() + "\n";
    } catch (...) {
        response["ok"] = false;
        response["error"] = "invalid_json";
        return response.dump() + "\n";
    }
}

void serve_client(SocketHandle client_fd, WebrtcPeerSession &peer)
{
    std::string pending;
    pending.reserve(4096);
    char buf[2048];

    while (g_running.load()) {
        const int n = recv(client_fd, buf, sizeof(buf), 0);
        if (n <= 0) {
            break;
        }

        pending.append(buf, static_cast<std::size_t>(n));
        for (;;) {
            const std::size_t pos = pending.find('\n');
            if (pos == std::string::npos) {
                break;
            }
            std::string line = pending.substr(0, pos);
            pending.erase(0, pos + 1);
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            if (line.empty()) {
                continue;
            }

            const std::string resp = line_response(peer, line);
            if (!send_all(client_fd, resp)) {
                return;
            }
        }
    }
}
} // namespace

int main(int argc, char **argv)
{
#ifndef _WIN32
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);
    std::signal(SIGPIPE, SIG_IGN);
#else
    std::signal(SIGINT, signal_handler);
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        std::cerr << "WSAStartup failed\n";
        return 1;
    }
#endif

    ServerConfig config;
    std::filesystem::path config_path = default_config_path();
    int cli_port_override = 0;
    bool has_port_override = false;
    bool self_check_only = false;

    for (int i = 1; i < argc; ++i) {
        const std::string opt = argv[i];
        auto need_value = [&](const std::string &name) -> std::string {
            if (i + 1 >= argc) {
                std::cerr << "missing value for " << name << '\n';
                return {};
            }
            return argv[++i];
        };

        if (opt == "-h" || opt == "--help") {
            print_usage(argv[0]);
#ifdef _WIN32
            WSACleanup();
#endif
            return 0;
        }
        if (opt == "--version") {
            std::cout << "release_webrtc_server 1.0.0\n";
#ifdef _WIN32
            WSACleanup();
#endif
            return 0;
        }
        if (opt == "--self-check-only") {
            self_check_only = true;
            continue;
        }
        if (opt == "-f" || opt == "--config") {
            const auto value = need_value(opt);
            if (value.empty()) {
#ifdef _WIN32
                WSACleanup();
#endif
                return 2;
            }
            config_path = value;
            continue;
        }
        if (opt == "--host") {
            const auto value = need_value(opt);
            if (value.empty()) {
#ifdef _WIN32
                WSACleanup();
#endif
                return 2;
            }
            config.host = value;
            continue;
        }
        if (opt == "--probe-host") {
            const auto value = need_value(opt);
            if (value.empty()) {
#ifdef _WIN32
                WSACleanup();
#endif
                return 2;
            }
            config.probe_host = value;
            continue;
        }
        if (opt == "-p" || opt == "--port") {
            const auto value = need_value(opt);
            if (value.empty() || !parse_int_value(value, cli_port_override)) {
                std::cerr << "invalid port: " << value << '\n';
#ifdef _WIN32
                WSACleanup();
#endif
                return 2;
            }
            has_port_override = true;
            continue;
        }
        if (!opt.empty() && opt[0] == '-') {
            std::cerr << "unknown option: " << opt << '\n';
            print_usage(argv[0]);
#ifdef _WIN32
            WSACleanup();
#endif
            return 2;
        }

        config_path = opt;
    }

    if (std::filesystem::exists(config_path)) {
        std::string error;
        if (!load_config_file(config_path, config, error)) {
            std::cerr << error << '\n';
#ifdef _WIN32
            WSACleanup();
#endif
            return 1;
        }
    }

    if (has_port_override) {
        config.port = cli_port_override;
    }
    config.port = read_env_int("YUAN_WEBRTC_PORT", config.port);

    if (config.port <= 0 || config.port > 65535) {
        std::cerr << "port out of range: " << config.port << '\n';
#ifdef _WIN32
        WSACleanup();
#endif
        return 1;
    }

    if (self_check_only) {
        if (probe_tcp(config.probe_host, config.port)) {
            std::cout << "self-check passed on " << config.probe_host << ':' << config.port << '\n';
#ifdef _WIN32
            WSACleanup();
#endif
            return 0;
        }
        std::cerr << "self-check failed on " << config.probe_host << ':' << config.port << '\n';
#ifdef _WIN32
        WSACleanup();
#endif
        return 1;
    }

    WebrtcPeerSession peer(config.local_ssrc, config.clock_rate);
    SignalQueueRuntimeConfig runtime_cfg;
    runtime_cfg.nomination_max_pending_signals = config.nomination_max_pending_signals;
    runtime_cfg.diagnostics_keep_latest_only = config.diagnostics_keep_latest_only;
    runtime_cfg.diagnostics_max_pending_signals = config.diagnostics_max_pending_signals;
    runtime_cfg.diagnostics_emit_flat_compat_fields = config.diagnostics_emit_flat_compat_fields;
    runtime_cfg.diagnostics_release_mode_strict_v2 = config.diagnostics_release_mode_strict_v2;
    runtime_cfg.diagnostics_rollout_alert_window_seconds = config.diagnostics_rollout_alert_window_seconds;
    runtime_cfg.diagnostics_rollout_mismatch_count_alert_threshold = config.diagnostics_rollout_mismatch_count_alert_threshold;
    runtime_cfg.diagnostics_rollout_mismatch_ratio_threshold_ppm = config.diagnostics_rollout_mismatch_ratio_threshold_ppm;
    peer.set_signal_queue_runtime_config(runtime_cfg);

    std::string listen_error;
    const SocketHandle listen_fd = create_listen_socket(config.host, config.port, listen_error);
    if (listen_fd == kInvalidSocket) {
        std::cerr << "failed to start release_webrtc_server on " << config.host << ':' << config.port
                  << " error=" << listen_error << '\n';
#ifdef _WIN32
        WSACleanup();
#endif
        return 1;
    }

    std::cout << "release_webrtc_server listening on " << config.host << ':' << config.port << '\n';
    std::cout << "strict_v2=" << (config.diagnostics_release_mode_strict_v2 ? "on" : "off")
              << " emit_flat=" << (config.diagnostics_emit_flat_compat_fields ? "on" : "off") << '\n';

    uint64_t next_health_log = now_ms() + static_cast<uint64_t>(config.health_log_interval_ms > 0 ? config.health_log_interval_ms : 5000);

    while (g_running.load()) {
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(listen_fd, &readfds);
        timeval tv{};
        tv.tv_sec = 0;
        tv.tv_usec = 200000;

        const int rv = select(static_cast<int>(listen_fd + 1), &readfds, nullptr, nullptr, &tv);
        if (rv > 0 && FD_ISSET(listen_fd, &readfds)) {
            sockaddr_storage addr{};
#ifdef _WIN32
            int addrlen = sizeof(addr);
#else
            socklen_t addrlen = sizeof(addr);
#endif
            const SocketHandle client_fd = accept(listen_fd, reinterpret_cast<sockaddr *>(&addr), &addrlen);
            if (client_fd != kInvalidSocket) {
                serve_client(client_fd, peer);
                close_socket(client_fd);
            }
        }

        if (config.health_log_interval_ms > 0 && now_ms() >= next_health_log) {
            std::cout << "health: " << peer.rollout_health_json() << '\n';
            next_health_log = now_ms() + static_cast<uint64_t>(config.health_log_interval_ms);
        }
    }

    close_socket(listen_fd);
#ifdef _WIN32
    WSACleanup();
#endif
    return 0;
}
