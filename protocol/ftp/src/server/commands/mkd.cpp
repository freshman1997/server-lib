#include "server/commands/mkd.h"
#include "common/response_code.h"
#include "common/session.h"
#include "server/command_support.h"
#include <filesystem>

namespace yuan::net::ftp
{
    FtpCommandResponse CommandMkd::execute(FtpSession *session, const std::string &args)
    {
        FtpCommandResponse denied{FtpResponseCode::invalid, ""};
        if (!ensure_login(session, denied)) {
            return denied;
        }
        const auto path = resolve_path(session, args);
        std::error_code ec;
        if (!path_within_root(session, path)) {
            return {FtpResponseCode::__550__, "Path is outside the FTP root."};
        }
        if (!std::filesystem::create_directories(path, ec) && ec) {
            return {FtpResponseCode::__550__, "Unable to create directory."};
        }
        return {FtpResponseCode::__257__, "\"" + to_virtual_path(session, path) + "\" created."};
    }

    CommandType CommandMkd::get_command_type() { return CommandType::cmd_mkd; }
    std::string CommandMkd::get_command_name() { return "MKD"; }
}
#include "server/command_support.h"
#include <filesystem>
