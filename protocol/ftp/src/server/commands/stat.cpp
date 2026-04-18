#include "server/commands/stat.h"
#include "common/response_code.h"
#include "common/session.h"
#include "server/command_support.h"
#include <filesystem>

namespace yuan::net::ftp
{
    FtpCommandResponse CommandStat::execute(FtpSession *session, const std::string &args)
    {
        FtpCommandResponse denied{FtpResponseCode::invalid, ""};
        if (!ensure_login(session, denied)) {
            return denied;
        }
        if (args.empty()) {
            return {FtpResponseCode::__211__, "FTP server status OK."};
        }
        const auto path = resolve_path(session, args);
        if (!path_within_root(session, path) || !std::filesystem::exists(path)) {
            return {FtpResponseCode::__550__, "Path is not available."};
        }
        return {FtpResponseCode::__213__, build_list_payload(path)};
    }

    CommandType CommandStat::get_command_type() { return CommandType::cmd_stat; }
    std::string CommandStat::get_command_name() { return "STAT"; }
}
#include "server/command_support.h"
#include <filesystem>
