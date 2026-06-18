#include "common/rpc_network.h"
#include "option.h"
#include "redis_client.h"

#include <nlohmann/json.hpp>

#ifndef _WIN32
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>

namespace
{
    bool require(bool condition, const char *message)
    {
        if (!condition) {
            std::cerr << message << '\n';
            return false;
        }
        return true;
    }

#ifndef _WIN32
    pid_t spawn_process(const std::string &path, const std::string &config_path)
    {
        const pid_t pid = ::fork();
        if (pid == 0) {
            ::execl(path.c_str(), path.c_str(), config_path.c_str(), static_cast<char *>(nullptr));
            _exit(127);
        }
        return pid;
    }

    bool wait_exit(pid_t pid, bool allow_sigterm = false)
    {
        int status = 0;
        if (::waitpid(pid, &status, 0) != pid) {
            return false;
        }
        if (WIFSIGNALED(status)) {
            return allow_sigterm && WTERMSIG(status) == SIGTERM;
        } else if (WIFEXITED(status) && WEXITSTATUS(status) != 0) {
            std::cerr << "process " << pid << " exited with status " << WEXITSTATUS(status) << '\n';
        }
        return WIFEXITED(status) && WEXITSTATUS(status) == 0;
    }

    void stop_process(pid_t pid)
    {
        if (pid > 0) {
            (void)::kill(pid, SIGTERM);
        }
    }
#endif

    void wait_startup()
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
    }

    bool write_file(const std::string &path, const std::string &content)
    {
        std::ofstream output(path);
        if (!output) {
            return false;
        }
        output << content;
        return static_cast<bool>(output);
    }

    std::string read_file(const std::string &path)
    {
        std::ifstream input(path);
        std::ostringstream buffer;
        buffer << input.rdbuf();
        return buffer.str();
    }

    nlohmann::json read_json_file(const std::string &path)
    {
        return nlohmann::json::parse(read_file(path));
    }

    bool write_json_file(const std::string &path, const nlohmann::json &json)
    {
        return write_file(path, json.dump(2));
    }
}

int main(int argc, char **argv)
{
#ifdef _WIN32
    (void)argc;
    (void)argv;
    std::cerr << "process smoke uses POSIX process spawning and is disabled on Windows\n";
    return EXIT_SUCCESS;
#else
    if (argc != 9) {
        std::cerr << "usage: test_game_server_smoke <global-bin> <tunnel-bin> <zone-bin> <world-bin> <gateway-bin> <player-db-proxy-bin> <world-db-proxy-bin> <mock-client-bin>\n";
        return 2;
    }

    const auto global_port = yuan::game::server::rpc_network::reserve_loopback_port();
    const auto zone_port = yuan::game::server::rpc_network::reserve_loopback_port();
    const auto tunnel_port = yuan::game::server::rpc_network::reserve_loopback_port();
    const auto world_port = yuan::game::server::rpc_network::reserve_loopback_port();
    const auto world_http_port = yuan::game::server::rpc_network::reserve_loopback_port();
    const auto gateway_port = yuan::game::server::rpc_network::reserve_loopback_port();
    const auto player_db_proxy_port = yuan::game::server::rpc_network::reserve_loopback_port();
    const auto world_db_proxy_port = yuan::game::server::rpc_network::reserve_loopback_port();
    if (!require(global_port != 0 && zone_port != 0 && tunnel_port != 0 && world_port != 0 && world_http_port != 0 && gateway_port != 0 && player_db_proxy_port != 0 && world_db_proxy_port != 0,
                 "ports should be reserved")) {
        return 3;
    }

    yuan::redis::Option redis_option;
    redis_option.host_ = "127.0.0.1";
    redis_option.port_ = 6379;
    redis_option.db_ = 0;
    redis_option.timeout_ms_ = 500;
    redis_option.connect_timeout_ms_ = 500;
    redis_option.name_ = "game-server-smoke-check";
    yuan::redis::RedisClient redis(redis_option);
    if (!redis.ensure_connected() || !redis.ping()) {
        std::cerr << "game_server_smoke skipped: local Redis 127.0.0.1:6379 unavailable\n";
        return EXIT_SUCCESS;
    }

    const std::string template_dir = GAME_SERVER_CONFIG_DIR;
    const std::string config_dir = "/tmp/opencode";
    const auto suffix = std::to_string(::getpid());
    const auto tunnel_config = config_dir + "/game_tunnel_" + suffix + ".json";
    const auto global_config = config_dir + "/game_global_" + suffix + ".json";
    const auto zone_config = config_dir + "/game_zone_" + suffix + ".json";
    const auto world_config = config_dir + "/game_world_" + suffix + ".json";
    const auto gateway_config = config_dir + "/game_gateway_" + suffix + ".json";
    const auto player_db_proxy_config = config_dir + "/game_player_db_proxy_" + suffix + ".json";
    const auto world_db_proxy_config = config_dir + "/game_world_db_proxy_" + suffix + ".json";
    auto tunnel_template = read_json_file(template_dir + "/tunnel.json");
    auto global_template = read_json_file(template_dir + "/global.json");
    auto zone_template = read_json_file(template_dir + "/zone.json");
    auto world_template = read_json_file(template_dir + "/world.json");
    auto gateway_template = read_json_file(template_dir + "/gateway.json");
    auto player_db_proxy_template = read_json_file(template_dir + "/player_db_proxy.json");
    auto world_db_proxy_template = read_json_file(template_dir + "/world_db_proxy.json");
    if (!require(!tunnel_template.empty() && !global_template.empty() && !zone_template.empty() && !world_template.empty() && !gateway_template.empty() && !player_db_proxy_template.empty() && !world_db_proxy_template.empty(), "config templates should load")) {
        return 15;
    }
    tunnel_template["listen_port"] = tunnel_port;
    global_template["listen_port"] = global_port;
    global_template["tunnel_endpoints"] = nlohmann::json::array({{{"host", "127.0.0.1"}, {"port", tunnel_port}}});
    zone_template["listen_port"] = zone_port;
    zone_template["tunnel_endpoints"] = nlohmann::json::array({{{"host", "127.0.0.1"}, {"port", tunnel_port}}});
    zone_template["gateway_endpoints"] = nlohmann::json::array({{{"service_id", 2252349813686273ULL}, {"host", "127.0.0.1"}, {"port", gateway_port}, {"name", "gateway"}}});
    zone_template["target_player_db_proxy_instance"] = 1;
    zone_template["player_db_proxies"] = nlohmann::json{{"strategy", "modulo"},
                                                         {"version", 1},
                                                         {"shard_count", 1},
                                                         {"endpoints", nlohmann::json::array({{{"service_id", 4504702360223745ULL}, {"shard", 0}, {"state", "open"}}})}};
    world_template["listen_port"] = world_port;
    world_template["http_port"] = world_http_port;
    world_template["tunnel_endpoints"] = nlohmann::json::array({{{"host", "127.0.0.1"}, {"port", tunnel_port}}});
    world_template["world_db_proxies"] = nlohmann::json{{"strategy", "modulo"},
                                                         {"version", 1},
                                                         {"shard_count", 1},
                                                         {"endpoints", nlohmann::json::array({{{"service_id", 4504702628659201ULL}, {"shard", 0}, {"state", "open"}}})}};
    gateway_template["listen_port"] = gateway_port;
    gateway_template["zone_endpoints"] = nlohmann::json::array({{{"service_id", 4504699675869185ULL}, {"host", "127.0.0.1"}, {"port", zone_port}}});
    player_db_proxy_template["listen_port"] = player_db_proxy_port;
    player_db_proxy_template["tunnel_endpoints"] = nlohmann::json::array({{{"host", "127.0.0.1"}, {"port", tunnel_port}}});
    world_db_proxy_template["listen_port"] = world_db_proxy_port;
    world_db_proxy_template["tunnel_endpoints"] = nlohmann::json::array({{{"host", "127.0.0.1"}, {"port", tunnel_port}}});

    if (!require(write_json_file(tunnel_config, tunnel_template), "tunnel config should write")) {
        return 12;
    }

    if (!require(write_json_file(global_config, global_template), "global config should write")) {
        return 13;
    }
    
    if (!require(write_json_file(zone_config, zone_template), "zone config should write")) {
        return 14;
    }

    if (!require(write_json_file(world_config, world_template), "world config should write") ||
        !require(write_json_file(player_db_proxy_config, player_db_proxy_template), "player db proxy config should write") ||
        !require(write_json_file(world_db_proxy_config, world_db_proxy_template), "world db proxy config should write") ||
        !require(write_json_file(gateway_config, gateway_template), "gateway config should write")) {
        return 16;
    }

    const pid_t tunnel_pid = spawn_process(argv[2], tunnel_config);
    if (!require(tunnel_pid > 0, "tunnel process should spawn")) {
        return 6;
    }
    wait_startup();

    const pid_t world_pid = spawn_process(argv[4], world_config);
    if (!require(world_pid > 0, "world process should spawn")) {
        return 17;
    }
    wait_startup();

    const pid_t player_db_proxy_pid = spawn_process(argv[6], player_db_proxy_config);
    if (!require(player_db_proxy_pid > 0, "player db proxy process should spawn")) {
        return 25;
    }
    wait_startup();

    const pid_t world_db_proxy_pid = spawn_process(argv[7], world_db_proxy_config);
    if (!require(world_db_proxy_pid > 0, "world db proxy process should spawn")) {
        return 27;
    }
    wait_startup();

    const pid_t zone_pid = spawn_process(argv[3], zone_config);
    if (!require(zone_pid > 0, "zone process should spawn")) {
        return 8;
    }
    wait_startup();

    const pid_t gateway_pid = spawn_process(argv[5], gateway_config);
    if (!require(gateway_pid > 0, "gateway process should spawn")) {
        return 18;
    }
    wait_startup();

    const auto player_uid_text = std::to_string(static_cast<unsigned long long>(
        std::chrono::steady_clock::now().time_since_epoch().count()));
    const pid_t client_pid = ::fork();
    if (client_pid == 0) {
        const auto world_http_port_text = std::to_string(world_http_port);
        const auto world_port_text = std::to_string(world_port);
        ::execl(argv[8], argv[8], world_http_port_text.c_str(), world_port_text.c_str(), player_uid_text.c_str(), static_cast<char *>(nullptr));
        _exit(127);
    }
    if (!require(client_pid > 0, "mock client should spawn")) {
        return 20;
    }

    const bool client_ok = wait_exit(client_pid);
    stop_process(gateway_pid);
    stop_process(zone_pid);
    stop_process(world_db_proxy_pid);
    stop_process(player_db_proxy_pid);
    stop_process(world_pid);
    stop_process(tunnel_pid);
    const bool gateway_ok = wait_exit(gateway_pid, client_ok);
    const bool zone_ok = wait_exit(zone_pid, client_ok);
    const bool world_db_proxy_ok = wait_exit(world_db_proxy_pid, client_ok);
    const bool player_db_proxy_ok = wait_exit(player_db_proxy_pid, client_ok);
    const bool world_ok = wait_exit(world_pid, client_ok);
    const bool tunnel_ok = wait_exit(tunnel_pid, client_ok);
    if (!require(client_ok, "mock client should exit successfully")) {
        return 21;
    }
    if (!require(gateway_ok, "gateway process should exit successfully")) {
        return 23;
    }
    if (!require(zone_ok, "zone process should exit successfully")) {
        return 9;
    }
    if (!require(player_db_proxy_ok, "player db proxy process should exit successfully")) {
        return 26;
    }
    if (!require(world_db_proxy_ok, "world db proxy process should exit successfully")) {
        return 28;
    }
    if (!require(world_ok, "world process should exit successfully")) {
        return 24;
    }
    if (!require(tunnel_ok, "tunnel process should exit successfully")) {
        return 10;
    }
    return EXIT_SUCCESS;
#endif
}
