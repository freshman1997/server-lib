#ifndef YUAN_RELEASE_SSH_CLI_FORWARD_H
#define YUAN_RELEASE_SSH_CLI_FORWARD_H

#include <cstdint>
#include <optional>
#include <string>

namespace yuan::release_ssh::client
{
    struct RemoteForwardSpec
    {
        std::string bind_addr;
        uint16_t bind_port = 0;
        std::string target_host;
        uint16_t target_port = 0;
    };

    using LocalForwardSpec = RemoteForwardSpec;

    struct DynamicForwardSpec
    {
        std::string bind_addr;
        uint16_t bind_port = 0;
    };

    std::optional<uint16_t> parse_u16_strict(const std::string &text);
    std::optional<LocalForwardSpec> parse_local_forward_spec(const std::string &raw);
    std::optional<DynamicForwardSpec> parse_dynamic_forward_spec(const std::string &raw);
    std::optional<RemoteForwardSpec> parse_remote_forward_spec(const std::string &raw);
}

#endif
