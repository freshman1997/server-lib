#include "ssh_cli_forward.h"

namespace yuan::release_ssh::client
{
    std::optional<uint16_t> parse_u16_strict(const std::string &text)
    {
        if (text.empty()) {
            return std::nullopt;
        }
        uint32_t value = 0;
        for (char ch : text) {
            if (ch < '0' || ch > '9') {
                return std::nullopt;
            }
            value = value * 10 + static_cast<uint32_t>(ch - '0');
            if (value > 65535) {
                return std::nullopt;
            }
        }
        return static_cast<uint16_t>(value);
    }

    std::optional<LocalForwardSpec> parse_local_forward_spec(const std::string &raw)
    {
        return parse_remote_forward_spec(raw);
    }

    std::optional<DynamicForwardSpec> parse_dynamic_forward_spec(const std::string &raw)
    {
        if (raw.empty()) {
            return std::nullopt;
        }

        DynamicForwardSpec spec;
        const size_t colon = raw.rfind(':');
        if (colon == std::string::npos) {
            auto port = parse_u16_strict(raw);
            if (!port || *port == 0) {
                return std::nullopt;
            }
            spec.bind_addr = "127.0.0.1";
            spec.bind_port = *port;
            return spec;
        }

        const std::string addr = raw.substr(0, colon);
        const std::string port_text = raw.substr(colon + 1);
        auto port = parse_u16_strict(port_text);
        if (!port || *port == 0) {
            return std::nullopt;
        }

        spec.bind_addr = addr.empty() ? "127.0.0.1" : addr;
        spec.bind_port = *port;
        return spec;
    }

    std::optional<RemoteForwardSpec> parse_remote_forward_spec(const std::string &raw)
    {
        const size_t c1 = raw.find(':');
        if (c1 == std::string::npos) {
            return std::nullopt;
        }
        const size_t c2 = raw.find(':', c1 + 1);
        if (c2 == std::string::npos) {
            return std::nullopt;
        }

        std::string first = raw.substr(0, c1);
        std::string second = raw.substr(c1 + 1, c2 - c1 - 1);
        std::string third = raw.substr(c2 + 1);

        if (second.empty() || third.empty()) {
            return std::nullopt;
        }

        const size_t c3 = third.rfind(':');
        if (c3 == std::string::npos || c3 == 0 || c3 + 1 >= third.size()) {
            return std::nullopt;
        }

        std::string target_host = third.substr(0, c3);
        std::string target_port_str = third.substr(c3 + 1);
        auto target_port = parse_u16_strict(target_port_str);
        if (!target_port || *target_port == 0) {
            return std::nullopt;
        }

        RemoteForwardSpec spec;
        if (auto maybe_mid_port = parse_u16_strict(second); maybe_mid_port) {
            spec.bind_addr = first.empty() ? "127.0.0.1" : first;
            spec.bind_port = *maybe_mid_port;
            if (spec.bind_port == 0) {
                return std::nullopt;
            }
            spec.target_host = target_host;
            spec.target_port = *target_port;
            return spec;
        }

        auto bind_port = parse_u16_strict(first);
        if (!bind_port || *bind_port == 0) {
            return std::nullopt;
        }

        spec.bind_addr = "127.0.0.1";
        spec.bind_port = *bind_port;
        spec.target_host = second;
        spec.target_port = *target_port;
        return spec;
    }
}
