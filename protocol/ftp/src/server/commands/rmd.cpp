#include "server/commands/rmd.h"
#include "common/response_code.h"
#include "common/session.h"
#include "server/command_support.h"
#include <filesystem>

namespace yuan::net::ftp
{
    FtpCommandResponse CommandRmd::execute(FtpSession *session, const std::string &args)
    {
        FtpCommandResponse denied{FtpResponseCode::invalid, ""};
        if (!ensure_login(session, denied)) {
            return denied;
        }
        const auto path = resolve_path(session, args);
        std::error_code ec;
        if (!path_within_root(session, path) || !std::filesystem::is_directory(path, ec)) {
            return {FtpResponseCode::__550__, "Directory is not available."};
        }
        if (!std::filesystem::remove(path, ec) || ec) {
            return {FtpResponseCode::__550__, "Unable to remove directory."};
        }
        return {FtpResponseCode::__250__, "Directory removed successfully."};
    }

    CommandType CommandRmd::get_command_type() { return CommandType::cmd_rmd; }
    std::string CommandRmd::get_command_name() { return "RMD"; }
}
#include "server/command_support.h"
#include <filesystem>
