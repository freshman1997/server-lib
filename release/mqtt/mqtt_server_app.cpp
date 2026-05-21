#include "mqtt_server_app.h"

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>

#include "nlohmann/json.hpp"

#include "mqtt_business_factory.h"
#include "mqtt_business_handler.h"
#include "mqtt_business_options.h"
#include "mqtt/mqtt_service.h"

namespace
{
    volatile std::sig_atomic_t g_running = 1;

    void signal_handler(int)
    {
        g_running = 0;
    }

    struct ServerConfig
    {
        int port = 1883;
        bool allow_anonymous = true;
        bool default_publish_allow = true;
        bool default_subscribe_allow = true;
        std::string retained_store_file = "mqtt_retained_store.json";
        std::string session_store_file = "mqtt_sessions.json";
        std::string policy_store_file = "mqtt_policy.json";
        int idle_timeout_ms = 30000;
        uint32_t max_packet_size = 256 * 1024;
        yuan::release::mqtt::MqttBusinessRouteOptions business_routes;
    };

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

    bool read_env_bool(const char *name, bool default_value)
    {
        const char *raw = std::getenv(name);
        if (!raw || *raw == '\0')
            return default_value;
        std::string v(raw);
        for (auto &ch : v)
            ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        if (v == "1" || v == "true" || v == "yes" || v == "on")
            return true;
        if (v == "0" || v == "false" || v == "no" || v == "off")
            return false;
        return default_value;
    }

    bool parse_int_value(const std::string &raw, int &out)
    {
        try {
            size_t pos = 0;
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

    bool parse_bool_value(const std::string &raw, bool &out)
    {
        std::string v = raw;
        for (auto &ch : v)
            ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        if (v == "1" || v == "true" || v == "yes" || v == "on") {
            out = true;
            return true;
        }
        if (v == "0" || v == "false" || v == "no" || v == "off") {
            out = false;
            return true;
        }
        return false;
    }

    std::filesystem::path default_config_path()
    {
        const auto env_path = read_env_string("YUAN_MQTT_CONFIG", "");
        if (!env_path.empty()) {
            return std::filesystem::path(env_path);
        }
        if (std::filesystem::exists(std::filesystem::path("release/mqtt/config.json"))) {
            return std::filesystem::path("release/mqtt/config.json");
        }
        return std::filesystem::path("config.json");
    }

    bool load_config_file(const std::filesystem::path &path, ServerConfig &config, std::string &error)
    {
        std::ifstream in(path);
        if (!in.good()) {
            error = "cannot open config file: " + path.string();
            return false;
        }

        nlohmann::json json;
        try {
            in >> json;
        } catch (const std::exception &ex) {
            error = std::string("invalid config json: ") + ex.what();
            return false;
        }

        if (json.contains("port") && json["port"].is_number_integer())
            config.port = json["port"].get<int>();
        if (json.contains("allow_anonymous") && json["allow_anonymous"].is_boolean())
            config.allow_anonymous = json["allow_anonymous"].get<bool>();
        if (json.contains("default_publish_allow") && json["default_publish_allow"].is_boolean())
            config.default_publish_allow = json["default_publish_allow"].get<bool>();
        if (json.contains("default_subscribe_allow") && json["default_subscribe_allow"].is_boolean())
            config.default_subscribe_allow = json["default_subscribe_allow"].get<bool>();
        if (json.contains("retained_store_file") && json["retained_store_file"].is_string())
            config.retained_store_file = json["retained_store_file"].get<std::string>();
        if (json.contains("session_store_file") && json["session_store_file"].is_string())
            config.session_store_file = json["session_store_file"].get<std::string>();
        if (json.contains("policy_store_file") && json["policy_store_file"].is_string())
            config.policy_store_file = json["policy_store_file"].get<std::string>();
        if (json.contains("idle_timeout_ms") && json["idle_timeout_ms"].is_number_integer())
            config.idle_timeout_ms = json["idle_timeout_ms"].get<int>();
        if (json.contains("max_packet_size") && json["max_packet_size"].is_number_unsigned())
            config.max_packet_size = json["max_packet_size"].get<uint32_t>();

        if (json.contains("business_routes") && json["business_routes"].is_object()) {
            const auto &routes = json["business_routes"];
            if (routes.contains("enable_publish_deny_route") && routes["enable_publish_deny_route"].is_boolean())
                config.business_routes.enable_publish_deny_route = routes["enable_publish_deny_route"].get<bool>();
            if (routes.contains("enable_publish_order_route") && routes["enable_publish_order_route"].is_boolean())
                config.business_routes.enable_publish_order_route = routes["enable_publish_order_route"].get<bool>();
            if (routes.contains("enable_publish_device_route") && routes["enable_publish_device_route"].is_boolean())
                config.business_routes.enable_publish_device_route = routes["enable_publish_device_route"].get<bool>();
            if (routes.contains("enable_publish_event_route") && routes["enable_publish_event_route"].is_boolean())
                config.business_routes.enable_publish_event_route = routes["enable_publish_event_route"].get<bool>();
            if (routes.contains("enable_subscribe_private_deny_route") && routes["enable_subscribe_private_deny_route"].is_boolean())
                config.business_routes.enable_subscribe_private_deny_route = routes["enable_subscribe_private_deny_route"].get<bool>();
            if (routes.contains("enable_subscribe_shared_audit_route") && routes["enable_subscribe_shared_audit_route"].is_boolean())
                config.business_routes.enable_subscribe_shared_audit_route = routes["enable_subscribe_shared_audit_route"].get<bool>();
        }

        return true;
    }

    void print_usage(const char *program)
    {
        std::cout
            << "release_mqtt_server\n"
            << "usage:\n"
            << "  " << program << " [--config <file>] [options]\n"
            << "  " << program << " <config.json>\n\n"
            << "options:\n"
            << "  -f, --config <file>          Read server config JSON\n"
            << "  -p, --port <port>            MQTT listen port\n"
            << "      --allow-anonymous <b>    Allow anonymous (true/false)\n"
            << "      --pub-default <b>        Default publish ACL allow\n"
            << "      --sub-default <b>        Default subscribe ACL allow\n"
            << "      --retained-file <file>   Retained store file\n"
            << "      --session-file <file>    Session store file\n"
            << "      --policy-file <file>     Policy store file\n"
            << "      --version                Print version\n"
            << "  -h, --help                   Show this help\n\n"
            << "env overrides:\n"
            << "  YUAN_MQTT_CONFIG, YUAN_MQTT_PORT, YUAN_MQTT_ALLOW_ANONYMOUS,\n"
            << "  YUAN_MQTT_PUB_DEFAULT_ALLOW, YUAN_MQTT_SUB_DEFAULT_ALLOW,\n"
            << "  YUAN_MQTT_RETAINED_FILE, YUAN_MQTT_SESSION_FILE, YUAN_MQTT_POLICY_FILE\n";
    }
}

namespace yuan::release::mqtt
{
    int ReleaseMqttServerApp::start(int argc, char **argv)
    {
#ifndef _WIN32
        std::signal(SIGINT, signal_handler);
        std::signal(SIGTERM, signal_handler);
        std::signal(SIGPIPE, SIG_IGN);
#endif

        ServerConfig config;
        std::filesystem::path config_path = default_config_path();

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
                return 0;
            }
            if (opt == "--version") {
                std::cout << "release_mqtt_server 1.0.0\n";
                return 0;
            }
            if (opt == "-f" || opt == "--config") {
                const auto value = need_value(opt);
                if (value.empty()) {
                    return 2;
                }
                config_path = value;
                continue;
            }
            if (opt == "-p" || opt == "--port") {
                const auto value = need_value(opt);
                int parsed = 0;
                if (value.empty() || !parse_int_value(value, parsed)) {
                    std::cerr << "invalid port: " << value << '\n';
                    return 2;
                }
                config.port = parsed;
                continue;
            }
            if (opt == "--allow-anonymous") {
                const auto value = need_value(opt);
                bool b = false;
                if (value.empty() || !parse_bool_value(value, b)) {
                    std::cerr << "invalid bool for --allow-anonymous\n";
                    return 2;
                }
                config.allow_anonymous = b;
                continue;
            }
            if (opt == "--pub-default") {
                const auto value = need_value(opt);
                bool b = false;
                if (value.empty() || !parse_bool_value(value, b)) {
                    std::cerr << "invalid bool for --pub-default\n";
                    return 2;
                }
                config.default_publish_allow = b;
                continue;
            }
            if (opt == "--sub-default") {
                const auto value = need_value(opt);
                bool b = false;
                if (value.empty() || !parse_bool_value(value, b)) {
                    std::cerr << "invalid bool for --sub-default\n";
                    return 2;
                }
                config.default_subscribe_allow = b;
                continue;
            }
            if (opt == "--retained-file") {
                config.retained_store_file = need_value(opt);
                if (config.retained_store_file.empty())
                    return 2;
                continue;
            }
            if (opt == "--session-file") {
                config.session_store_file = need_value(opt);
                if (config.session_store_file.empty())
                    return 2;
                continue;
            }
            if (opt == "--policy-file") {
                config.policy_store_file = need_value(opt);
                if (config.policy_store_file.empty())
                    return 2;
                continue;
            }

            if (!opt.empty() && opt[0] == '-') {
                std::cerr << "unknown option: " << opt << '\n';
                print_usage(argv[0]);
                return 2;
            }

            config_path = opt;
        }

        if (std::filesystem::exists(config_path)) {
            std::string error;
            if (!load_config_file(config_path, config, error)) {
                std::cerr << error << '\n';
                return 1;
            }
        }

        config.port = read_env_int("YUAN_MQTT_PORT", config.port);
        config.allow_anonymous = read_env_bool("YUAN_MQTT_ALLOW_ANONYMOUS", config.allow_anonymous);
        config.default_publish_allow = read_env_bool("YUAN_MQTT_PUB_DEFAULT_ALLOW", config.default_publish_allow);
        config.default_subscribe_allow = read_env_bool("YUAN_MQTT_SUB_DEFAULT_ALLOW", config.default_subscribe_allow);
        config.retained_store_file = read_env_string("YUAN_MQTT_RETAINED_FILE", config.retained_store_file);
        config.session_store_file = read_env_string("YUAN_MQTT_SESSION_FILE", config.session_store_file);
        config.policy_store_file = read_env_string("YUAN_MQTT_POLICY_FILE", config.policy_store_file);
        config.business_routes.enable_publish_deny_route = read_env_bool("YUAN_MQTT_BIZ_PUBLISH_DENY_ROUTE", config.business_routes.enable_publish_deny_route);
        config.business_routes.enable_publish_order_route = read_env_bool("YUAN_MQTT_BIZ_PUBLISH_ORDER_ROUTE", config.business_routes.enable_publish_order_route);
        config.business_routes.enable_publish_device_route = read_env_bool("YUAN_MQTT_BIZ_PUBLISH_DEVICE_ROUTE", config.business_routes.enable_publish_device_route);
        config.business_routes.enable_publish_event_route = read_env_bool("YUAN_MQTT_BIZ_PUBLISH_EVENT_ROUTE", config.business_routes.enable_publish_event_route);
        config.business_routes.enable_subscribe_private_deny_route = read_env_bool("YUAN_MQTT_BIZ_SUB_PRIVATE_DENY_ROUTE", config.business_routes.enable_subscribe_private_deny_route);
        config.business_routes.enable_subscribe_shared_audit_route = read_env_bool("YUAN_MQTT_BIZ_SUB_SHARED_AUDIT_ROUTE", config.business_routes.enable_subscribe_shared_audit_route);

        if (config.port <= 0 || config.port > 65535) {
            std::cerr << "port out of range: " << config.port << '\n';
            return 1;
        }

        yuan::net::mqtt::MqttServerConfig mqtt_cfg;
        mqtt_cfg.port = static_cast<uint16_t>(config.port);
        mqtt_cfg.idle_timeout_ms = static_cast<uint32_t>(std::max(0, config.idle_timeout_ms));
        mqtt_cfg.max_packet_size = config.max_packet_size;

        yuan::server::MqttService service(static_cast<int>(mqtt_cfg.port), mqtt_cfg);
        service.use_enhanced_handler(false);

        auto &eh = service.enhanced_handler();
        eh.set_allow_anonymous(config.allow_anonymous);
        eh.set_default_publish_allow(config.default_publish_allow);
        eh.set_default_subscribe_allow(config.default_subscribe_allow);

        auto business_handler = create_mqtt_business_handler(config.business_routes);
        CompositeMqttHandler composite_handler(&eh, business_handler.get());
        service.set_handler(&composite_handler);

        (void)service.load_policy_store(config.policy_store_file);
        (void)service.load_retained_store(config.retained_store_file);
        (void)service.load_session_store(config.session_store_file);

        if (!service.init()) {
            std::cerr << "failed to init mqtt service on port " << config.port << '\n';
            return 1;
        }

        std::cout << "release_mqtt_server listening on 0.0.0.0:" << config.port << '\n';
        std::cout << "allow anonymous: " << (config.allow_anonymous ? "true" : "false") << '\n';
        std::cout << "policy file: " << config.policy_store_file << '\n';
        std::cout << "retained file: " << config.retained_store_file << '\n';
        std::cout << "session file: " << config.session_store_file << '\n';

        service.start();

        while (g_running) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }

        (void)service.save_policy_store(config.policy_store_file);
        (void)service.save_session_store(config.session_store_file);
        (void)service.save_retained_store(config.retained_store_file);

        service.stop();
        return 0;
    }
}
