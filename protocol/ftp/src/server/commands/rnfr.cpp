#include "server/commands/rnfr.h"
#include "common/response_code.h"
#include "common/session.h"
#include "server/command_support.h"
#include <filesystem>

namespace yuan::net::ftp
{
    FtpCommandResponse CommandRnfr::execute(FtpSession *session, const std::string &args)
    {
        FtpCommandResponse denied{FtpResponseCode::invalid, ""};
        if (!ensure_login(session, denied)) {
            return denied;
        }
        const auto path = resolve_path(session, args);
        std::error_code ec;
        if (!path_within_root(session, path) || !std::filesystem::exists(path, ec)) {
            return {FtpResponseCode::__550__, "File is not available."};
        }
        session->set_item_value<std::string>("rename_from", path.generic_string());
        return {FtpResponseCode::__350__, "File exists, ready for destination name."};
    }

    CommandType CommandRnfr::get_command_type() { return CommandType::cmd_rnfr; }
    std::string CommandRnfr::get_command_name() { return "RNFR"; }
}
#include "server/command_support.h"
#include <filesystem>
