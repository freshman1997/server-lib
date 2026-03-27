#include "server/commands/pasv.h"
#include "common/response_code.h"
#include "common/session.h"
#include "net/socket/inet_address.h"
#include "server/command_support.h"
#include "server/context.h"
namespace yuan::net::ftp
{
    REGISTER_COMMAND_IMPL(CommandPasv);
    FtpCommandResponse CommandPasv::execute(FtpSession *session, const std::string &args)
    {
        (void)args;
        FtpCommandResponse denied{FtpResponseCode::invalid, ""};
        if (!ensure_login(session, denied)) {
            return denied;
        }
        const short port = ServerContext::get_instance()->get_next_stream_port();
        if (port <= 0) {
            return {FtpResponseCode::__425__, "No passive ports available."};
        }
        InetAddress addr("", port);
        if (!session->start_file_stream(addr, StreamMode::Receiver)) {
            ServerContext::get_instance()->remove_stream_port(port);
            return {FtpResponseCode::__425__, "Can't open passive data listener."};
        }
        session->set_passive_addr(addr);
        return {FtpResponseCode::__227__, build_pasv_response("127.0.0.1", port)};
    }
    CommandType CommandPasv::get_command_type() { return CommandType::cmd_pasv; }
    std::string CommandPasv::get_comand_name() { return "PASV"; }
}
