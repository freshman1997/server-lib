#include "server/commands/noop.h"
#include "common/response_code.h"
namespace yuan::net::ftp
{
    FtpCommandResponse CommandNoop::execute(FtpSession *session, const std::string &args)
    {
        (void)session;
        (void)args;
        return {FtpResponseCode::__200__, "NOOP ok."};
    }
    CommandType CommandNoop::get_command_type() { return CommandType::cmd_noop; }
    std::string CommandNoop::get_command_name() { return "NOOP"; }
}
