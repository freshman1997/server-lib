#include "server/commands/pwd.h"
#include "common/response_code.h"
#include "common/session.h"
#include "server/command_support.h"
namespace yuan::net::ftp
{
    REGISTER_COMMAND_IMPL(CommandPwd);
    FtpCommandResponse CommandPwd::execute(FtpSession *session, const std::string &args)
    {
        (void)args;
        FtpCommandResponse denied{FtpResponseCode::invalid, ""};
        if (!ensure_login(session, denied)) {
            return denied;
        }
        return {FtpResponseCode::__257__, "\"" + session->get_cwd() + "\" is the current directory."};
    }
    CommandType CommandPwd::get_command_type() { return CommandType::cmd_cwd; }
    std::string CommandPwd::get_comand_name() { return "PWD"; }
}
