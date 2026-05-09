#include "server/commands/port.h"
#include "common/response_code.h"
#include "common/session.h"
#include "server/command_support.h"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <vector>

namespace yuan::net::ftp
{
    namespace
    {
        bool parse_active_addr(const std::string &args, InetAddress &out)
        {
            std::string payload = args;
            payload.erase(std::remove_if(payload.begin(), payload.end(), [](unsigned char ch) {
                return std::isspace(ch) != 0;
            }), payload.end());

            std::stringstream ss(payload);
            std::string token;
            std::vector<int> values;
            while (std::getline(ss, token, ',')) {
                try {
                    int v = std::stoi(token);
                    if (v < 0 || v > 255) {
                        return false;
                    }
                    values.push_back(v);
                } catch (...) {
                    return false;
                }
            }

            if (values.size() != 6) {
                return false;
            }

            const std::string ip =
                std::to_string(values[0]) + "." +
                std::to_string(values[1]) + "." +
                std::to_string(values[2]) + "." +
                std::to_string(values[3]);
            const int port = values[4] * 256 + values[5];
            if (port <= 0 || port > 65535) {
                return false;
            }

            out = InetAddress(ip, static_cast<uint16_t>(port));
            return true;
        }
    }

    FtpCommandResponse CommandPort::execute(FtpSession *session, const std::string &args)
    {
        FtpCommandResponse denied{FtpResponseCode::invalid, ""};
        if (!ensure_login(session, denied)) {
            return denied;
        }

        InetAddress active_addr;
        if (!parse_active_addr(args, active_addr)) {
            return {FtpResponseCode::__501__, "Syntax error in PORT arguments."};
        }

        session->set_active_addr(active_addr);
        session->clear_passive_addr();
        return {FtpResponseCode::__200__, "PORT command successful."};
    }

    CommandType CommandPort::get_command_type() { return CommandType::cmd_port; }
    std::string CommandPort::get_command_name() { return "PORT"; }
}
