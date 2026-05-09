#include "server/commands/size.h"
#include "common/response_code.h"
#include "common/session.h"
#include "server/command_support.h"
#include <filesystem>

namespace yuan::net::ftp
{
    FtpCommandResponse CommandSize::execute(FtpSession *session, const std::string &args)
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
        return {FtpResponseCode::__213__, std::to_string(std::filesystem::file_size(path, ec))};
    }

    CommandType CommandSize::get_command_type() { return CommandType::cmd_size; }
    std::string CommandSize::get_command_name() { return "SIZE"; }
}
