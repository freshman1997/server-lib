#include "server/commands/syst.h"
#include "common/response_code.h"
namespace yuan::net::ftp
{
    REGISTER_COMMAND_IMPL(CommandSyst);
    FtpCommandResponse CommandSyst::execute(FtpSession *session, const std::string &args)
    {
        (void)session;
        (void)args;
        return {FtpResponseCode::__215__, "UNIX Type: L8"};
    }
    CommandType CommandSyst::get_command_type() { return CommandType::cmd_help; }
    std::string CommandSyst::get_comand_name() { return "SYST"; }
}
