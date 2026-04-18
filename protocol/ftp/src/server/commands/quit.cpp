#include "server/commands/quit.h"
#include "common/response_code.h"
#include "common/session.h"

namespace yuan::net::ftp
{
    FtpCommandResponse CommandQuit::execute(FtpSession *session, const std::string &args)
    {
        (void)args;
        if (session) {
            session->quit();
        }
        return {FtpResponseCode::__221__, "Goodbye.", true};
    }

    CommandType CommandQuit::get_command_type() { return CommandType::cmd_quit; }
    std::string CommandQuit::get_command_name() { return "QUIT"; }
}
