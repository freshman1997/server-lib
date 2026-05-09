#include "server/commands/type.h"
#include "common/response_code.h"

#include <algorithm>
#include <cctype>

namespace yuan::net::ftp
{
    FtpCommandResponse CommandTypeCmd::execute(FtpSession *session, const std::string &args)
    {
        (void)session;
        std::string type = args;
        std::transform(type.begin(), type.end(), type.begin(), [](unsigned char ch) {
            return static_cast<char>(std::toupper(ch));
        });
        if (type != "A" && type != "I") {
            return {FtpResponseCode::__504__, "Only ASCII and Image types are supported."};
        }
        return {FtpResponseCode::__200__, "Type set successfully."};
    }

    CommandType CommandTypeCmd::get_command_type() { return CommandType::cmd_type; }
    std::string CommandTypeCmd::get_command_name() { return "TYPE"; }
}
