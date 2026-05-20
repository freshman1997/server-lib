#include "server/commands/feat.h"

#include "common/response_code.h"

namespace yuan::net::ftp
{
    FtpCommandResponse CommandFeat::execute(FtpSession *session, const std::string &args)
    {
        (void)session;
        (void)args;
        return {FtpResponseCode::__211__,
                "Features:\n"
                "UTF8\n"
                "EPSV\n"
                "EPRT\n"
                "PASV\n"
                "PORT\n"
                "SIZE\n"
                "REST STREAM\n"
                "APPE",
                false,
                true};
    }

    CommandType CommandFeat::get_command_type()
    {
        return CommandType::cmd_feat;
    }

    std::string CommandFeat::get_command_name()
    {
        return "FEAT";
    }
}
