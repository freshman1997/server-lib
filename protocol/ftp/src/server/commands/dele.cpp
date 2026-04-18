#include "server/commands/dele.h"
#include "common/response_code.h"
#include "common/session.h"
#include "server/command_support.h"
#include <filesystem>

namespace yuan::net::ftp
{
    FtpCommandResponse CommandDele::execute(FtpSession *session, const std::string &args)
    {
        FtpCommandResponse denied{FtpResponseCode::invalid, ""};
        if (!ensure_login(session, denied)) {
            return denied;
        }
        const auto path = resolve_path(session, args);
        std::error_code ec;
        if (!path_within_root(session, path) || !std::filesystem::is_regular_file(path, ec)) {
            return {FtpResponseCode::__550__, "File is not available."};
        }
        if (!std::filesystem::remove(path, ec) || ec) {
            return {FtpResponseCode::__450__, "Unable to delete file."};
        }
        return {FtpResponseCode::__250__, "File deleted successfully."};
    }

    CommandType CommandDele::get_command_type() { return CommandType::cmd_dele; }
    std::string CommandDele::get_command_name() { return "DELE"; }
}
#include "server/command_support.h"
#include <filesystem>
