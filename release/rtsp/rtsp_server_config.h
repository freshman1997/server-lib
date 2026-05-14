#ifndef __RELEASE_RTSP_SERVER_CONFIG_H__
#define __RELEASE_RTSP_SERVER_CONFIG_H__

#include <cstddef>
#include <cstdint>
#include <string>

namespace yuan::release::rtsp
{

struct ServerConfig
{
    int port = 554;
    std::string app_name = "release-rtsp-server";
    bool enable_log = true;
    bool enable_audit = true;
    std::size_t max_audit_events = 256;
    uint32_t udp_retry_max_retries = 2;
    uint64_t udp_retry_base_backoff_ms = 25;
    uint64_t udp_retry_max_backoff_ms = 1000;
};

enum class ParseMode
{
    run,
    print_help,
    print_version,
};

struct ParseResult
{
    ParseMode mode = ParseMode::run;
    ServerConfig config;
};

bool parse_server_options(int argc, char **argv, ParseResult &out, std::string &error);
void print_usage(const char *program);
const char *version_string();

} // namespace yuan::release::rtsp

#endif
