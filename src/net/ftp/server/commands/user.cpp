#include "net/ftp/common/response_code.h"
#include "net/ftp/common/session.h"
#include "net/ftp/server/commands/user.h"

#include <string>

namespace net::ftp 
{
    REGISTER_COMMAND_IMPL(CommandUser);

    FtpCommandResponse CommandUser::execute(FtpSession *session, const std::string &args)
    {
        // TODO 登录处理
        if (args.empty()) {
            return {FtpResponseCode::__503__, "invalid args"};
        }
        return {FtpResponseCode::__230__, "login successfully"};
    }

    CommandType CommandUser::get_command_type()
    {
        return CommandType::cmd_user;
    }

    std::string CommandUser::get_comand_name()
    {
        return "USER";
    }
}