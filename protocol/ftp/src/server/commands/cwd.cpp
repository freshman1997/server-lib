#include "server/commands/cwd.h"
#include "common/response_code.h"
#include "common/session.h"
#include "server/command_support.h"
#include <filesystem>
namespace yuan::net::ftp
{
    REGISTER_COMMAND_IMPL(CommandCwd);
    FtpCommandResponse CommandCwd::execute(FtpSession *session, const std::string &args)
    {
        FtpCommandResponse denied{FtpResponseCode::invalid, ""};
        if (!ensure_login(session, denied)) {
            return denied;
        }
        const auto target = resolve_path(session, args);
        if (!path_within_root(session, target) || !std::filesystem::is_directory(target)) {
            return {FtpResponseCode::__550__, "Directory is not available."};
        }
        session->change_cwd(to_virtual_path(session, target));
        return {FtpResponseCode::__250__, "Directory changed successfully."};
    }
    CommandType CommandCwd::get_command_type() { return CommandType::cmd_cwd; }
    std::string CommandCwd::get_comand_name() { return "CWD"; }
}
