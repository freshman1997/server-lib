#include "server/commands/rnto.h"
#include "common/response_code.h"
#include "common/session.h"
#include "server/command_support.h"
#include <filesystem>

namespace yuan::net::ftp
{
    FtpCommandResponse CommandRnto::execute(FtpSession *session, const std::string &args)
    {
        FtpCommandResponse denied{FtpResponseCode::invalid, ""};
        if (!ensure_login(session, denied)) {
            return denied;
        }
        auto *from = session->get_item_value<std::string *>("rename_from");
        if (!from || from->empty()) {
            return {FtpResponseCode::__503__, "Send RNFR before RNTO."};
        }
        const auto from_path = std::filesystem::path(*from);
        const auto to_path = resolve_path(session, args);
        std::error_code ec;
        if (!path_within_root(session, from_path) || !path_within_root(session, to_path)) {
            session->remove_item("rename_from");
            return {FtpResponseCode::__550__, "Rename path is outside the FTP root."};
        }
        std::filesystem::create_directories(to_path.parent_path(), ec);
        ec.clear();
        std::filesystem::rename(from_path, to_path, ec);
        session->remove_item("rename_from");
        if (ec) {
            return {FtpResponseCode::__550__, "Rename failed."};
        }
        return {FtpResponseCode::__250__, "Rename successful."};
    }

    CommandType CommandRnto::get_command_type() { return CommandType::cmd_rnto; }
    std::string CommandRnto::get_command_name() { return "RNTO"; }
}
#include "server/command_support.h"
#include <filesystem>
