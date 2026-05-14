#include "rtsp_server_config.h"

#include "nlohmann/json.hpp"

#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>

namespace yuan::release::rtsp
{
namespace
{

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
    if (!raw || *raw == '\0') {
        return default_value;
    }
    std::string value(raw);
    for (char &ch : value) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    if (value == "1" || value == "true" || value == "yes" || value == "on") {
        return true;
    }
    if (value == "0" || value == "false" || value == "no" || value == "off") {
        return false;
    }
    return default_value;
}

uint32_t read_env_u32(const char *name, uint32_t default_value)
{
    const char *raw = std::getenv(name);
    if (!raw || *raw == '\0') {
        return default_value;
    }
    try {
        size_t pos = 0;
        const unsigned long value = std::stoul(raw, &pos);
        if (raw[pos] != '\0') {
            return default_value;
        }
        return static_cast<uint32_t>(value);
    } catch (...) {
        return default_value;
    }
}

uint64_t read_env_u64(const char *name, uint64_t default_value)
{
    const char *raw = std::getenv(name);
    if (!raw || *raw == '\0') {
        return default_value;
    }
    try {
        size_t pos = 0;
        const unsigned long long value = std::stoull(raw, &pos);
        if (raw[pos] != '\0') {
            return default_value;
        }
        return static_cast<uint64_t>(value);
    } catch (...) {
        return default_value;
    }
}

std::size_t read_env_size(const char *name, std::size_t default_value)
{
    const char *raw = std::getenv(name);
    if (!raw || *raw == '\0') {
        return default_value;
    }
    try {
        size_t pos = 0;
        const unsigned long long value = std::stoull(raw, &pos);
        if (raw[pos] != '\0') {
            return default_value;
        }
        return static_cast<std::size_t>(value);
    } catch (...) {
        return default_value;
    }
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
    std::string value = raw;
    for (char &ch : value) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }

    if (value == "1" || value == "true" || value == "yes" || value == "on") {
        out = true;
        return true;
    }
    if (value == "0" || value == "false" || value == "no" || value == "off") {
        out = false;
        return true;
    }
    return false;
}

bool parse_size_value(const std::string &raw, std::size_t &out)
{
    try {
        size_t pos = 0;
        const unsigned long long value = std::stoull(raw, &pos);
        if (pos != raw.size()) {
            return false;
        }
        out = static_cast<std::size_t>(value);
        return true;
    } catch (...) {
        return false;
    }
}

bool parse_u32_value(const std::string &raw, uint32_t &out)
{
    try {
        size_t pos = 0;
        const unsigned long value = std::stoul(raw, &pos);
        if (pos != raw.size()) {
            return false;
        }
        out = static_cast<uint32_t>(value);
        return true;
    } catch (...) {
        return false;
    }
}

bool parse_u64_value(const std::string &raw, uint64_t &out)
{
    try {
        size_t pos = 0;
        const unsigned long long value = std::stoull(raw, &pos);
        if (pos != raw.size()) {
            return false;
        }
        out = static_cast<uint64_t>(value);
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

    nlohmann::json json;
    try {
        in >> json;
    } catch (const std::exception &ex) {
        error = std::string("invalid config json: ") + ex.what();
        return false;
    }

    if (json.contains("port") && json["port"].is_number_integer()) {
        config.port = json["port"].get<int>();
    }
    if (json.contains("app_name") && json["app_name"].is_string()) {
        config.app_name = json["app_name"].get<std::string>();
    }
    if (json.contains("enable_log") && json["enable_log"].is_boolean()) {
        config.enable_log = json["enable_log"].get<bool>();
    }
    if (json.contains("enable_audit") && json["enable_audit"].is_boolean()) {
        config.enable_audit = json["enable_audit"].get<bool>();
    }
    if (json.contains("max_audit_events") && json["max_audit_events"].is_number_unsigned()) {
        config.max_audit_events = json["max_audit_events"].get<std::size_t>();
    }
    if (json.contains("udp_retry_max_retries") && json["udp_retry_max_retries"].is_number_unsigned()) {
        config.udp_retry_max_retries = json["udp_retry_max_retries"].get<uint32_t>();
    }
    if (json.contains("udp_retry_base_backoff_ms") && json["udp_retry_base_backoff_ms"].is_number_unsigned()) {
        config.udp_retry_base_backoff_ms = json["udp_retry_base_backoff_ms"].get<uint64_t>();
    }
    if (json.contains("udp_retry_max_backoff_ms") && json["udp_retry_max_backoff_ms"].is_number_unsigned()) {
        config.udp_retry_max_backoff_ms = json["udp_retry_max_backoff_ms"].get<uint64_t>();
    }

    return true;
}

std::filesystem::path default_config_path()
{
    const auto env_path = read_env_string("YUAN_RTSP_CONFIG", "");
    if (!env_path.empty()) {
        return std::filesystem::path(env_path);
    }
    if (std::filesystem::exists(std::filesystem::path("release/rtsp/config.json"))) {
        return std::filesystem::path("release/rtsp/config.json");
    }
    return std::filesystem::path("config.json");
}

} // namespace

const char *version_string()
{
    return "release_rtsp_server 1.0.0";
}

void print_usage(const char *program)
{
    std::cout
        << "release_rtsp_server\n"
        << "usage:\n"
        << "  " << program << " [--config <file>] [options]\n"
        << "  " << program << " <config.json>\n\n"
        << "options:\n"
        << "  -f, --config <file>                Read server config JSON\n"
        << "  -p, --port <port>                  RTSP listen port\n"
        << "      --app-name <name>              Display app name\n"
        << "      --enable-log <bool>            Enable request log\n"
        << "      --enable-audit <bool>          Enable in-memory audit events\n"
        << "      --max-audit-events <count>     Max retained audit events\n"
        << "      --udp-retry-max <count>        UDP retry max retries\n"
        << "      --udp-retry-base-ms <ms>       UDP retry base backoff milliseconds\n"
        << "      --udp-retry-max-ms <ms>        UDP retry max backoff milliseconds\n"
        << "      --version                      Print version\n"
        << "  -h, --help                         Show this help\n\n"
        << "env overrides:\n"
        << "  YUAN_RTSP_CONFIG, YUAN_RTSP_PORT, YUAN_RTSP_APP_NAME,\n"
        << "  YUAN_RTSP_ENABLE_LOG, YUAN_RTSP_ENABLE_AUDIT, YUAN_RTSP_MAX_AUDIT_EVENTS,\n"
        << "  YUAN_RTSP_UDP_RETRY_MAX, YUAN_RTSP_UDP_RETRY_BASE_MS, YUAN_RTSP_UDP_RETRY_MAX_MS\n";
}

bool parse_server_options(int argc, char **argv, ParseResult &out, std::string &error)
{
    ServerConfig config;
    ServerConfig cli_overrides;
    bool has_port_override = false;
    bool has_app_name_override = false;
    bool has_enable_log_override = false;
    bool has_enable_audit_override = false;
    bool has_max_audit_override = false;
    bool has_udp_retry_max_override = false;
    bool has_udp_retry_base_override = false;
    bool has_udp_retry_max_backoff_override = false;
    std::filesystem::path config_path = default_config_path();

    for (int i = 1; i < argc; ++i) {
        const std::string opt = argv[i];
        auto need_value = [&](const std::string &name) -> std::string {
            if (i + 1 >= argc) {
                error = "missing value for " + name;
                return {};
            }
            return argv[++i];
        };

        if (opt == "-h" || opt == "--help") {
            out.mode = ParseMode::print_help;
            return true;
        }
        if (opt == "--version") {
            out.mode = ParseMode::print_version;
            return true;
        }
        if (opt == "-f" || opt == "--config") {
            const auto value = need_value(opt);
            if (value.empty()) {
                return false;
            }
            config_path = value;
            continue;
        }
        if (opt == "-p" || opt == "--port") {
            const auto value = need_value(opt);
            if (value.empty() || !parse_int_value(value, cli_overrides.port)) {
                error = "invalid port: " + value;
                return false;
            }
            has_port_override = true;
            continue;
        }
        if (opt == "--app-name") {
            cli_overrides.app_name = need_value(opt);
            if (cli_overrides.app_name.empty()) {
                return false;
            }
            has_app_name_override = true;
            continue;
        }
        if (opt == "--enable-log") {
            const auto value = need_value(opt);
            if (!parse_bool_value(value, cli_overrides.enable_log)) {
                error = "invalid boolean for --enable-log: " + value;
                return false;
            }
            has_enable_log_override = true;
            continue;
        }
        if (opt == "--enable-audit") {
            const auto value = need_value(opt);
            if (!parse_bool_value(value, cli_overrides.enable_audit)) {
                error = "invalid boolean for --enable-audit: " + value;
                return false;
            }
            has_enable_audit_override = true;
            continue;
        }
        if (opt == "--max-audit-events") {
            const auto value = need_value(opt);
            if (value.empty() || !parse_size_value(value, cli_overrides.max_audit_events)) {
                error = "invalid max audit events: " + value;
                return false;
            }
            has_max_audit_override = true;
            continue;
        }
        if (opt == "--udp-retry-max") {
            const auto value = need_value(opt);
            if (value.empty() || !parse_u32_value(value, cli_overrides.udp_retry_max_retries)) {
                error = "invalid udp retry max: " + value;
                return false;
            }
            has_udp_retry_max_override = true;
            continue;
        }
        if (opt == "--udp-retry-base-ms") {
            const auto value = need_value(opt);
            if (value.empty() || !parse_u64_value(value, cli_overrides.udp_retry_base_backoff_ms)) {
                error = "invalid udp retry base backoff ms: " + value;
                return false;
            }
            has_udp_retry_base_override = true;
            continue;
        }
        if (opt == "--udp-retry-max-ms") {
            const auto value = need_value(opt);
            if (value.empty() || !parse_u64_value(value, cli_overrides.udp_retry_max_backoff_ms)) {
                error = "invalid udp retry max backoff ms: " + value;
                return false;
            }
            has_udp_retry_max_backoff_override = true;
            continue;
        }
        if (!opt.empty() && opt[0] == '-') {
            error = "unknown option: " + opt;
            return false;
        }

        config_path = opt;
    }

    if (std::filesystem::exists(config_path)) {
        if (!load_config_file(config_path, config, error)) {
            return false;
        }
    }

    if (has_port_override) {
        config.port = cli_overrides.port;
    }
    if (has_app_name_override) {
        config.app_name = cli_overrides.app_name;
    }
    if (has_enable_log_override) {
        config.enable_log = cli_overrides.enable_log;
    }
    if (has_enable_audit_override) {
        config.enable_audit = cli_overrides.enable_audit;
    }
    if (has_max_audit_override) {
        config.max_audit_events = cli_overrides.max_audit_events;
    }
    if (has_udp_retry_max_override) {
        config.udp_retry_max_retries = cli_overrides.udp_retry_max_retries;
    }
    if (has_udp_retry_base_override) {
        config.udp_retry_base_backoff_ms = cli_overrides.udp_retry_base_backoff_ms;
    }
    if (has_udp_retry_max_backoff_override) {
        config.udp_retry_max_backoff_ms = cli_overrides.udp_retry_max_backoff_ms;
    }

    config.port = read_env_int("YUAN_RTSP_PORT", config.port);
    config.app_name = read_env_string("YUAN_RTSP_APP_NAME", config.app_name);
    config.enable_log = read_env_bool("YUAN_RTSP_ENABLE_LOG", config.enable_log);
    config.enable_audit = read_env_bool("YUAN_RTSP_ENABLE_AUDIT", config.enable_audit);
    config.max_audit_events = read_env_size("YUAN_RTSP_MAX_AUDIT_EVENTS", config.max_audit_events);
    config.udp_retry_max_retries = read_env_u32("YUAN_RTSP_UDP_RETRY_MAX", config.udp_retry_max_retries);
    config.udp_retry_base_backoff_ms = read_env_u64("YUAN_RTSP_UDP_RETRY_BASE_MS", config.udp_retry_base_backoff_ms);
    config.udp_retry_max_backoff_ms = read_env_u64("YUAN_RTSP_UDP_RETRY_MAX_MS", config.udp_retry_max_backoff_ms);

    if (config.port <= 0 || config.port > 65535) {
        error = "port out of range: " + std::to_string(config.port);
        return false;
    }
    if (config.max_audit_events == 0) {
        config.max_audit_events = 1;
    }
    if (config.udp_retry_base_backoff_ms == 0) {
        config.udp_retry_base_backoff_ms = 1;
    }
    if (config.udp_retry_max_backoff_ms < config.udp_retry_base_backoff_ms) {
        config.udp_retry_max_backoff_ms = config.udp_retry_base_backoff_ms;
    }

    out.mode = ParseMode::run;
    out.config = config;
    return true;
}

} // namespace yuan::release::rtsp
