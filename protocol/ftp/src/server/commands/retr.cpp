#include "server/commands/retr.h"
#include "common/response_code.h"

namespace net::ftp 
{
    REGISTER_COMMAND_IMPL(CommandRetr);

    FtpCommandResponse CommandRetr::execute(FtpSession *session, const std::string &args)
    {
        return {FtpResponseCode::__227__, "192.168.96.1 12124"};
    }

    CommandType CommandRetr::get_command_type()
    {
        return CommandType::cmd_retr;
    }

    std::string CommandRetr::get_comand_name()
    {
        return "RETR";
    }
}