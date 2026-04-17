#include "transport/ssh_version_exchange.h"
#include "protocol/ssh_constants.h"
#include <cstring>

namespace yuan::net::ssh
{
    std::string SshVersionExchange::build_server_version(const std::string & software_version)
    {
        return "SSH-2.0-" + software_version + "\r\n";
    }

    std::optional<SshVersionInfo> SshVersionExchange::parse_version_line(const std::string & line)
    {
        if (line.size() < 4)
            return std::nullopt;
        if (line.substr(0, 4) != "SSH-")
            return std::nullopt;

        auto dash1 = line.find('-', 4);
        if (dash1 == std::string::npos)
            return std::nullopt;

        SshVersionInfo info;
        info.protocol_version = line.substr(4, dash1 - 4);

        auto sp = line.find(' ', dash1 + 1);
        auto cr = line.find('\r', dash1 + 1);

        size_t software_end = line.size();
        if (sp != std::string::npos && sp < software_end)
            software_end = sp;
        if (cr != std::string::npos && cr < software_end)
            software_end = cr;

        info.software_version = line.substr(dash1 + 1, software_end - dash1 - 1);

        if (sp != std::string::npos && sp < cr) {
            size_t comment_end = cr != std::string::npos ? cr : line.size();
            info.comment = line.substr(sp + 1, comment_end - sp - 1);
        }

        info.raw_line = line;
        if (!info.raw_line.empty() && info.raw_line.back() == '\n')
            info.raw_line.pop_back();
        if (!info.raw_line.empty() && info.raw_line.back() == '\r')
            info.raw_line.pop_back();

        return info;
    }

    bool SshVersionExchange::is_valid_protocol_version(const std::string & version)
    {
        if (version.size() < 3)
            return false;
        if (version[0] != '2')
            return false;
        if (version == "2.0" || version == "1.99")
            return true;
        return false;
    }

    std::optional<size_t> SshVersionExchange::find_version_line_end(const uint8_t * data, size_t len)
    {
        for (size_t i = 0; i < len - 1; ++i) {
            if (data[i] == '\r' && data[i + 1] == '\n') {
                return i + 2;
            }
            if (data[i] == '\n') {
                return i + 1;
            }
        }
        if (len > 0 && data[len - 1] == '\n') {
            return len;
        }
        return std::nullopt;
    }
}
