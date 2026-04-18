#include "server/commands/site.h"
#include "common/response_code.h"

namespace yuan::net::ftp
{
    FtpCommandResponse CommandSite::execute(FtpSession *session, const std::string &args)
    {
        (void)session;
        (void)args;
        return {FtpResponseCode::__502__, "SITE is not supported."};
    }

    CommandType CommandSite::get_command_type() { return CommandType::cmd_site; }
    std::string CommandSite::get_command_name() { return "SITE"; }
}
#include "common/response_code.h"
