#include "server/commands/epsv.h"

#include "common/response_code.h"
#include "common/session.h"
#include "server/command_support.h"
#include "server/context.h"

namespace yuan::net::ftp
{
    FtpCommandResponse CommandEpsv::execute(FtpSession *session, const std::string &args)
    {
        FtpCommandResponse denied{FtpResponseCode::invalid, ""};
        if (!ensure_login(session, denied)) {
            return denied;
        }

        if (!args.empty() && args != "1" && args != "2") {
            return {FtpResponseCode::__522__, "Network protocol not supported, use (1,2)."};
        }

        const short port = ServerContext::get_instance()->get_next_stream_port();
        if (port <= 0) {
            return {FtpResponseCode::__425__, "No passive ports available."};
        }

        auto *conn = session->get_connection();
        std::string local_ip = conn ? conn->get_local_address().get_ip() : "";
        InetAddress addr(local_ip, port);
        if (!session->start_file_stream(addr, StreamMode::Receiver)) {
            ServerContext::get_instance()->remove_stream_port(port);
            return {FtpResponseCode::__425__, "Can't open passive data listener."};
        }

        session->set_passive_addr(addr);
        session->clear_active_addr();
        return {FtpResponseCode::__229__, "Entering Extended Passive Mode (|||" + std::to_string(port) + "|)"};
    }

    CommandType CommandEpsv::get_command_type()
    {
        return CommandType::cmd_epsv;
    }

    std::string CommandEpsv::get_command_name()
    {
        return "EPSV";
    }
}
