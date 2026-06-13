#include "common/service_config.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>

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
}

int main()
{
    namespace fs = std::filesystem;
    using namespace yuan::game::server;

    const auto path = fs::temp_directory_path() / "game_server_world_config_test.json";
    {
        std::ofstream out(path);
        out << R"json({
  "type": 9,
  "region": 1,
  "world": 1,
  "instance": 1,
  "listen_host": "127.0.0.1",
  "listen_port": 25003,
  "http_port": 25103,
  "tunnel_endpoints": [
    { "host": "127.0.0.1", "port": 25000 }
  ],
  "world_ownership_store": "redis",
  "client_frame_max_bytes": 4096,
  "client_frame_max_per_window": 30,
  "client_frame_rate_window_ms": 500,
  "rpc_max_connections": 2000,
  "rpc_max_buffered_bytes": 131072,
  "rpc_idle_timeout_ms": 30000
})json";
    }

    const auto config = load_service_server_config(path.string());
    std::error_code ignored;
    fs::remove(path, ignored);

    if (!require(config.has_value(), "world config should parse")) {
        return 1;
    }
    if (!require(config->world_ownership_store == "redis", "world ownership store mode should parse")) {
        return 2;
    }
    if (!require(config->tunnel_endpoints.size() == 1, "world tunnel endpoint should parse")) {
        return 3;
    }
    if (!require(config->client_frame_max_bytes == 4096 && config->client_frame_max_per_window == 30 && config->client_frame_rate_window_ms == 500,
                 "client frame policy should parse")) {
        return 4;
    }
    if (!require(config->rpc_max_connections == 2000 && config->rpc_max_buffered_bytes == 131072 && config->rpc_idle_timeout_ms == 30000,
                 "rpc lifecycle policy should parse")) {
        return 5;
    }
    return EXIT_SUCCESS;
}
