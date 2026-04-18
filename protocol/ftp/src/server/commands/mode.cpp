#include "server/commands/mode.h"
#include "common/response_code.h"

#include <algorithm>
#include <cctype>

namespace yuan::net::ftp
{
    FtpCommandResponse CommandMode::execute(FtpSession *session, const std::string &args)
    {
        (void)session;
        std::string type = args;
        std::transform(type.begin(), type.end(), type.begin(), [](unsigned char ch) {
            return static_cast<char>(std::toupper(ch));
        });
        if (type != "S") {
            return {FtpResponseCode::__504__, "Only stream mode is supported."};
        }
        return {FtpResponseCode::__200__, "Mode set to Stream."};
    }

    CommandType CommandMode::get_command_type() { return CommandType::cmd_mode; }
    std::string CommandMode::get_command_name() { return "MODE"; }
}
#include "common/response_code.h"
