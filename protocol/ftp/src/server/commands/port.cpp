#include "server/commands/port.h"
#include "common/response_code.h"
namespace yuan::net::ftp
{
    REGISTER_COMMAND_IMPL(CommandPort);
    FtpCommandResponse CommandPort::execute(FtpSession *session, const std::string &args)
    {
        (void)session;
        (void)args;
        return {FtpResponseCode::__502__, "PORT active mode is not implemented."};
    }
    CommandType CommandPort::get_command_type() { return CommandType::cmd_port; }
    std::string CommandPort::get_comand_name() { return "PORT"; }
}
