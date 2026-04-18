#include "server/commands/stru.h"
#include "common/response_code.h"

#include <algorithm>
#include <cctype>

namespace yuan::net::ftp
{
    FtpCommandResponse CommandStru::execute(FtpSession *session, const std::string &args)
    {
        (void)session;
        std::string type = args;
        std::transform(type.begin(), type.end(), type.begin(), [](unsigned char ch) {
            return static_cast<char>(std::toupper(ch));
        });
        if (type != "F") {
            return {FtpResponseCode::__504__, "Only file structure is supported."};
        }
        return {FtpResponseCode::__200__, "Structure set to File."};
    }

    CommandType CommandStru::get_command_type() { return CommandType::cmd_stru; }
    std::string CommandStru::get_command_name() { return "STRU"; }
}
#include "common/response_code.h"
