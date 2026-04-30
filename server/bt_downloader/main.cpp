#include "application.h"
#include "bit_torrent_service.h"
#include "bootstrap.h"
#include "http_service.h"

#include "nlohmann/json.hpp"

#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <thread>

namespace
{
    volatile std::sig_atomic_t g_should_exit = 0;

    void signal_handler(int)
    {
        g_should_exit = 1;
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

    std::string read_env_string(const char *name, const std::string &default_value = {})
    {
        const char *raw = std::getenv(name);
        return raw ? std::string(raw) : default_value;
    }

    bool read_env_bool(const char *name, bool default_value)
    {
        const char *raw = std::getenv(name);
        if (!raw || *raw == '\0') {
            return default_value;
        }
        std::string s(raw);
        if (s == "1" || s == "true" || s == "yes" || s == "on") {
            return true;
        }
        if (s == "0" || s == "false" || s == "no" || s == "off") {
            return false;
        }
        return default_value;
    }

    struct DownloaderConfig
    {
        int admin_port = 18080;
        std::string torrent_file;
        std::string save_path = ".";
        bool enable_keep_alive = true;
        bool enable_cors = true;
        std::string app_name = "bt_downloader";
        int bt_max_peers = 50;
        int bt_listen_port = 6881;
        int bt_listen_port_end = 6999;
        int bt_download_limit_kbps = 0;
        int bt_upload_limit_kbps = 0;
        int bt_max_concurrent_downloads = 3;
        bool bt_enable_dht = true;
        bool bt_enable_pex = true;
        bool bt_enable_upnp = true;
    };

    bool load_config_file(const std::filesystem::path &path, DownloaderConfig &cfg, std::string &error)
    {
        std::ifstream in(path);
        if (!in.good()) {
            error = "cannot open config file: " + path.string();
            return false;
        }

        nlohmann::json j;
        try {
            in >> j;
        } catch (const std::exception &e) {
            error = std::string("invalid config json: ") + e.what();
            return false;
        }

        if (j.contains("admin_port") && j["admin_port"].is_number_integer()) {
            cfg.admin_port = j["admin_port"].get<int>();
        }
        if (j.contains("torrent_file") && j["torrent_file"].is_string()) {
            cfg.torrent_file = j["torrent_file"].get<std::string>();
        }
        if (j.contains("save_path") && j["save_path"].is_string()) {
            cfg.save_path = j["save_path"].get<std::string>();
        }
        if (j.contains("enable_keep_alive") && j["enable_keep_alive"].is_boolean()) {
            cfg.enable_keep_alive = j["enable_keep_alive"].get<bool>();
        }
        if (j.contains("enable_cors") && j["enable_cors"].is_boolean()) {
            cfg.enable_cors = j["enable_cors"].get<bool>();
        }
        if (j.contains("app_name") && j["app_name"].is_string()) {
            cfg.app_name = j["app_name"].get<std::string>();
        }

        if (j.contains("bt") && j["bt"].is_object()) {
            const auto &bt = j["bt"];
            if (bt.contains("max_peers") && bt["max_peers"].is_number_integer()) {
                cfg.bt_max_peers = bt["max_peers"].get<int>();
            }
            if (bt.contains("listen_port") && bt["listen_port"].is_number_integer()) {
                cfg.bt_listen_port = bt["listen_port"].get<int>();
            }
            if (bt.contains("listen_port_end") && bt["listen_port_end"].is_number_integer()) {
                cfg.bt_listen_port_end = bt["listen_port_end"].get<int>();
            }
            if (bt.contains("download_limit_kbps") && bt["download_limit_kbps"].is_number_integer()) {
                cfg.bt_download_limit_kbps = bt["download_limit_kbps"].get<int>();
            }
            if (bt.contains("upload_limit_kbps") && bt["upload_limit_kbps"].is_number_integer()) {
                cfg.bt_upload_limit_kbps = bt["upload_limit_kbps"].get<int>();
            }
            if (bt.contains("enable_dht") && bt["enable_dht"].is_boolean()) {
                cfg.bt_enable_dht = bt["enable_dht"].get<bool>();
            }
            if (bt.contains("enable_pex") && bt["enable_pex"].is_boolean()) {
                cfg.bt_enable_pex = bt["enable_pex"].get<bool>();
            }
            if (bt.contains("enable_upnp") && bt["enable_upnp"].is_boolean()) {
                cfg.bt_enable_upnp = bt["enable_upnp"].get<bool>();
            }
            if (bt.contains("max_concurrent_downloads") && bt["max_concurrent_downloads"].is_number_integer()) {
                cfg.bt_max_concurrent_downloads = bt["max_concurrent_downloads"].get<int>();
            }
        }

        return true;
    }

    std::optional<std::filesystem::path> resolve_config_path(int argc, char **argv)
    {
        if (argc > 1 && argv[1] && *argv[1] != '\0') {
            return std::filesystem::path(argv[1]);
        }

        const auto env_path = read_env_string("YUAN_BT_CONFIG", "");
        if (!env_path.empty()) {
            return std::filesystem::path(env_path);
        }

        const std::filesystem::path default_path("server/bt_downloader/config.json");
        if (std::filesystem::exists(default_path)) {
            return default_path;
        }

        return std::nullopt;
    }
}

int main(int argc, char **argv)
{
#ifndef _WIN32
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);
    std::signal(SIGPIPE, SIG_IGN);
#endif

    DownloaderConfig cfg;

    if (const auto config_path = resolve_config_path(argc, argv)) {
        std::string error;
        if (!load_config_file(*config_path, cfg, error)) {
            std::cerr << error << "\n";
            return 1;
        }
    }

    cfg.admin_port = read_env_int("YUAN_BT_ADMIN_PORT", cfg.admin_port);
    cfg.torrent_file = read_env_string("YUAN_BT_TORRENT_FILE", cfg.torrent_file);
    cfg.save_path = read_env_string("YUAN_BT_SAVE_PATH", cfg.save_path);
    cfg.bt_max_peers = read_env_int("YUAN_BT_MAX_PEERS", cfg.bt_max_peers);
    cfg.bt_listen_port = read_env_int("YUAN_BT_LISTEN_PORT", cfg.bt_listen_port);
    cfg.bt_listen_port_end = read_env_int("YUAN_BT_LISTEN_PORT_END", cfg.bt_listen_port_end);
    cfg.bt_download_limit_kbps = read_env_int("YUAN_BT_DOWNLOAD_LIMIT_KBPS", cfg.bt_download_limit_kbps);
    cfg.bt_upload_limit_kbps = read_env_int("YUAN_BT_UPLOAD_LIMIT_KBPS", cfg.bt_upload_limit_kbps);
    cfg.bt_enable_dht = read_env_bool("YUAN_BT_ENABLE_DHT", cfg.bt_enable_dht);
    cfg.bt_enable_pex = read_env_bool("YUAN_BT_ENABLE_PEX", cfg.bt_enable_pex);
    cfg.bt_enable_upnp = read_env_bool("YUAN_BT_ENABLE_UPNP", cfg.bt_enable_upnp);
    cfg.bt_max_concurrent_downloads = read_env_int("YUAN_BT_MAX_CONCURRENT", cfg.bt_max_concurrent_downloads);

    yuan::net::http::HttpServerConfig http_config;
    http_config.enable_keep_alive = cfg.enable_keep_alive;
    http_config.enable_cors = cfg.enable_cors;

    yuan::app::RuntimeContext context;
    context.app_name = cfg.app_name;
    context.run_mode = yuan::app::RunMode::single_thread;
    context.worker_threads = 1;

    yuan::app::Application application(context);

    auto http_service = std::make_shared<yuan::server::HttpService>(cfg.admin_port, http_config);
    if (!application.add_typed_service<yuan::server::HttpService>(
            "http",
            http_service,
            "server.http",
            1)) {
        std::cerr << "failed to register http service\n";
        return 1;
    }

    auto bt_service = std::make_shared<yuan::server::BitTorrentService>(cfg.torrent_file, cfg.save_path);
    bt_service->set_max_concurrent_downloads(cfg.bt_max_concurrent_downloads);
    bt_service->set_default_max_peers(cfg.bt_max_peers);
    bt_service->set_default_listen_port(cfg.bt_listen_port);
    bt_service->set_default_listen_port_end(cfg.bt_listen_port_end);
    bt_service->set_default_download_limit_kbps(cfg.bt_download_limit_kbps);
    bt_service->set_default_upload_limit_kbps(cfg.bt_upload_limit_kbps);
    {
        yuan::net::bit_torrent::NatConfig nat_cfg;
        nat_cfg.enable_dht = cfg.bt_enable_dht;
        nat_cfg.enable_pex = cfg.bt_enable_pex;
        nat_cfg.enable_upnp = cfg.bt_enable_upnp;
        nat_cfg.enable_nat_pmp = cfg.bt_enable_upnp;
        bt_service->set_default_nat_config(nat_cfg);
    }
    if (!application.add_typed_service<yuan::server::BitTorrentService>(
            "bittorrent",
            bt_service,
            "server.bittorrent",
            1)) {
        std::cerr << "failed to register bittorrent service\n";
        return 1;
    }

    yuan::app::Bootstrap bootstrap(application);
    if (!bootstrap.run()) {
        std::cerr << "bootstrap run failed\n";
        return 1;
    }

    std::cout << "bt_downloader started. admin=http://127.0.0.1:" << cfg.admin_port << "/admin\n";
    std::cout << "env: YUAN_BT_CONFIG, YUAN_BT_ADMIN_PORT, YUAN_BT_TORRENT_FILE, YUAN_BT_SAVE_PATH, YUAN_BT_MAX_PEERS, YUAN_BT_LISTEN_PORT, YUAN_BT_DOWNLOAD_LIMIT_KBPS, YUAN_BT_UPLOAD_LIMIT_KBPS, YUAN_BT_ENABLE_DHT, YUAN_BT_ENABLE_PEX, YUAN_BT_ENABLE_UPNP, YUAN_BT_MAX_CONCURRENT, YUAN_ADMIN_TOKEN\n";

    while (!g_should_exit) {
        bootstrap.poll_workers();
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    bootstrap.shutdown();
    return 0;
}
