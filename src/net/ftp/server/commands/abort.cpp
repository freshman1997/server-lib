#include "net/ftp/server/commands/abort.h"
#include "net/ftp/common/response_code.h"
#include "net/ftp/common/session.h"

namespace net::ftp 
{
    REGISTER_COMMAND_IMPL(CommandAbort);

    FtpCommandResponse CommandAbort::execute(FtpSession *session, const std::string &args)
    {
        return {FtpResponseCode::__221__, "Ok session exit", true};
    }

    CommandType CommandAbort::get_command_type()
    {
        return CommandType::cmd_abort;
    }

    std::string CommandAbort::get_comand_name()
    {
        return "ABORT";
    }
}