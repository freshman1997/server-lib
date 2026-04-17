#ifndef __NET_SSH_TRANSPORT_SSH_VERSION_EXCHANGE_H__
#define __NET_SSH_TRANSPORT_SSH_VERSION_EXCHANGE_H__

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace yuan::net::ssh
{
    struct SshVersionInfo
    {
        std::string protocol_version;
        std::string software_version;
        std::string comment;
        std::string raw_line;
    };

    class SshVersionExchange
    {
    public:
        static std::string build_server_version(const std::string &software_version);

        static std::optional<SshVersionInfo> parse_version_line(const std::string &line);

        static bool is_valid_protocol_version(const std::string &version);

        static std::optional<size_t> find_version_line_end(const uint8_t *data, size_t len);

        static constexpr size_t kMaxVersionLineLen = 253;
        static constexpr size_t kMaxBannerLines = 1024;
    };
}

#endif
