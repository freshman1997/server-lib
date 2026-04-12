#include "log_config.h"
#include <cstdio>
#include <string>

namespace yuan::log
{

namespace json_helper
{

static const char* skip_ws(const char* p)
{
    while (*p && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) ++p;
    return p;
}

static std::string parse_string(const char*& p)
{
    p = skip_ws(p);
    if (*p != '"') return {};

    ++p;
    std::string s;
    while (*p && *p != '"') {
        if (*p == '\\' && *(p + 1)) {
            ++p;
        }
        s += *p;
        ++p;
    }
    if (*p == '"') ++p;
    return s;
}

static std::string parse_value_raw(const char*& p)
{
    p = skip_ws(p);
    if (*p == '"') return parse_string(p);
    if (*p == '{' || *p == '[') return {};

    const char* start = p;
    while (*p && *p != ',' && *p != '}' && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r') {
        ++p;
    }
    return std::string(start, static_cast<size_t>(p - start));
}

static bool find_key_value(const char* json, const char* key, std::string& out_val)
{
    const char* p = json;
    while (*p) {
        p = skip_ws(p);
        if (*p == '"') {
            auto k = parse_string(p);
            p = skip_ws(p);
            if (*p == ':') {
                ++p;
            } else {
                continue;
            }

            if (k == key) {
                out_val = parse_value_raw(p);
                return true;
            }

            parse_value_raw(p);
            p = skip_ws(p);
            if (*p == ',') ++p;
        } else if (*p == '}') {
            break;
        } else {
            ++p;
        }
    }
    return false;
}

static bool find_sub_object(const char* json, const char* key, const char*& sub_start, const char*& sub_end)
{
    const char* p = json;
    while (*p) {
        p = skip_ws(p);
        if (*p == '"') {
            auto k = parse_string(p);
            p = skip_ws(p);
            if (*p != ':') continue;
            ++p;

            if (k == key) {
                p = skip_ws(p);
                if (*p != '{') return false;
                sub_start = p;

                int depth = 1;
                ++p;
                while (*p && depth > 0) {
                    if (*p == '{') ++depth;
                    else if (*p == '}') --depth;
                    ++p;
                }
                sub_end = p - 1;
                return true;
            }

            parse_value_raw(p);
            p = skip_ws(p);
            if (*p == ',') ++p;
        } else if (*p == '}') {
            break;
        } else {
            ++p;
        }
    }
    return false;
}

static int to_int(const std::string& s) { return std::stoi(s); }
static uint64_t to_uint64(const std::string& s) { return std::stoull(s); }
static bool to_bool(const std::string& s) { return s == "true" || s == "1"; }

} // namespace json_helper

LogConfig LogConfig::default_config()
{
    return LogConfig{};
}

LogConfig LogConfig::load_from_json(const std::string& json_path)
{
    FILE* f = nullptr;
#ifdef _WIN32
    if (fopen_s(&f, json_path.c_str(), "rb") != 0) {
        f = nullptr;
    }
#else
    f = std::fopen(json_path.c_str(), "rb");
#endif
    if (!f) return default_config();

    std::fseek(f, 0, SEEK_END);
    const long len = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (len <= 0) {
        std::fclose(f);
        return default_config();
    }

    std::string content(static_cast<size_t>(len), '\0');
    std::fread(content.data(), 1, static_cast<size_t>(len), f);
    std::fclose(f);

    return parse_from_json_string(content);
}

LogConfig LogConfig::parse_from_json_string(const std::string& json_str)
{
    using namespace json_helper;

    auto read_string = [](const char* json, const char* key, std::string& target) {
        std::string val;
        if (find_key_value(json, key, val)) target = val;
    };
    auto read_int = [](const char* json, const char* key, int& target) {
        std::string val;
        if (find_key_value(json, key, val)) target = to_int(val);
    };
    auto read_uint64 = [](const char* json, const char* key, uint64_t& target) {
        std::string val;
        if (find_key_value(json, key, val)) target = to_uint64(val);
    };
    auto read_bool = [](const char* json, const char* key, bool& target) {
        std::string val;
        if (find_key_value(json, key, val)) target = to_bool(val);
    };

    LogConfig cfg;
    const char* root = json_str.c_str();

    read_string(root, "version", cfg.version);
    read_string(root, "log_path", cfg.log_path);
    read_string(root, "log_file_name", cfg.log_file_name);

    std::string val;
    if (find_key_value(root, "log_level", val)) {
        cfg.log_level = str_to_level(val);
    }

    read_bool(root, "async_mode", cfg.async_mode);
    read_string(root, "net_server_ip", cfg.net_server_ip);
    read_int(root, "net_server_port", cfg.net_server_port);
    read_bool(root, "net_auto_reconnect", cfg.net_auto_reconnect);
    read_int(root, "net_connect_timeout_ms", cfg.net_connect_timeout_ms);
    read_int(root, "net_reconnect_delay_ms", cfg.net_reconnect_delay_ms);
    read_int(root, "net_max_retries", cfg.net_max_retries);
    read_int(root, "net_max_pending_messages", cfg.net_max_pending_messages);
    read_bool(root, "net_drop_oldest_on_overflow", cfg.net_drop_oldest_on_overflow);
    read_string(root, "fmt", cfg.fmt_pattern);
    read_string(root, "fmt_datefmt", cfg.fmt_datefmt);

    const char* rotate_start = nullptr;
    const char* rotate_end = nullptr;
    if (find_sub_object(root, "rotate", rotate_start, rotate_end)) {
        std::string rotate_json(rotate_start, static_cast<size_t>(rotate_end - rotate_start + 1));
        const char* rotate = rotate_json.c_str();

        if (find_key_value(rotate, "policy", val)) {
            if (val == "size" || val == "S") {
                cfg.rotate_policy = RotatePolicy::size_limit;
            } else if (val == "hourly" || val == "H") {
                cfg.rotate_policy = RotatePolicy::hourly;
            } else if (val == "daily" || val == "D") {
                cfg.rotate_policy = RotatePolicy::daily;
            } else {
                cfg.rotate_policy = RotatePolicy::none;
            }
        }

        read_uint64(rotate, "max_file_size", cfg.max_file_size);
        read_int(rotate, "backup_count", cfg.backup_count);
        read_string(rotate, "encoding", cfg.encoding);
    }

    return cfg;
}

} // namespace yuan::log
