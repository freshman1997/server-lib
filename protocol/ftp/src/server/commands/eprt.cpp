#include "server/commands/eprt.h"

#include "common/response_code.h"
#include "common/session.h"
#include "server/command_support.h"

#include <string>

namespace yuan::net::ftp
{
    namespace
    {
        bool parse_eprt_endpoint(const std::string &args, net::InetAddress &out)
        {
            if (args.size() < 7) {
                return false;
            }

            const char delimiter = args.front();
            if (delimiter != '|' && delimiter != '!') {
                return false;
            }

            if (args.back() != delimiter) {
                return false;
            }

            std::string payload = args.substr(1, args.size() - 2);
            size_t p1 = payload.find(delimiter);
            if (p1 == std::string::npos) {
                return false;
            }
            size_t p2 = payload.find(delimiter, p1 + 1);
            if (p2 == std::string::npos) {
                return false;
            }

            const std::string af = payload.substr(0, p1);
            const std::string host = payload.substr(p1 + 1, p2 - p1 - 1);
            const std::string port_text = payload.substr(p2 + 1);

            if (host.empty() || port_text.empty()) {
                return false;
            }

            int port = 0;
            try {
                size_t used = 0;
                port = std::stoi(port_text, &used);
                if (used != port_text.size()) {
                    return false;
                }
            } catch (...) {
                return false;
            }

            if (port <= 0 || port > 65535) {
                return false;
            }

            if (af != "1" && af != "2") {
                return false;
            }

            out = net::InetAddress(host, port);
            return true;
        }
    }

    FtpCommandResponse CommandEprt::execute(FtpSession *session, const std::string &args)
    {
        FtpCommandResponse denied{FtpResponseCode::invalid, ""};
        if (!ensure_login(session, denied)) {
            return denied;
        }

        net::InetAddress active_addr;
        if (!parse_eprt_endpoint(args, active_addr)) {
            return {FtpResponseCode::__501__, "Syntax error in EPRT arguments."};
        }

        session->set_active_addr(active_addr);
        session->clear_passive_addr();
        return {FtpResponseCode::__200__, "EPRT command successful."};
    }

    CommandType CommandEprt::get_command_type()
    {
        return CommandType::cmd_eprt;
    }

    std::string CommandEprt::get_command_name()
    {
        return "EPRT";
    }
}
