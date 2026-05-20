#include "server/commands/opts.h"

#include "common/response_code.h"

#include <algorithm>
#include <cctype>

namespace yuan::net::ftp
{
    FtpCommandResponse CommandOpts::execute(FtpSession *session, const std::string &args)
    {
        (void)session;
        std::string value = args;
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
            return static_cast<char>(std::toupper(ch));
        });

        if (value == "UTF8 ON" || value == "UTF-8 ON") {
            return {FtpResponseCode::__200__, "UTF8 option enabled."};
        }
        return {FtpResponseCode::__504__, "Unsupported option."};
    }

    CommandType CommandOpts::get_command_type()
    {
        return CommandType::cmd_opts;
    }

    std::string CommandOpts::get_command_name()
    {
        return "OPTS";
    }
}
