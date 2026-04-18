#include "server/commands/acct.h"
#include "common/response_code.h"

namespace yuan::net::ftp
{
    FtpCommandResponse CommandAcct::execute(FtpSession *session, const std::string &args)
    {
        (void)session;
        (void)args;
        return {FtpResponseCode::__502__, "ACCT is not supported."};
    }

    CommandType CommandAcct::get_command_type() { return CommandType::cmd_acct; }
    std::string CommandAcct::get_command_name() { return "ACCT"; }
}
#include "common/response_code.h"
