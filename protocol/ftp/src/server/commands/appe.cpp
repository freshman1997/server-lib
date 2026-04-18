#include "server/commands/appe.h"
#include "common/response_code.h"
#include "common/session.h"
#include "server/command_support.h"
#include <filesystem>

namespace yuan::net::ftp
{
    FtpCommandResponse CommandAppe::execute(FtpSession *session, const std::string &args)
    {
        FtpCommandResponse denied{FtpResponseCode::invalid, ""};
        if (!ensure_login(session, denied)) {
            return denied;
        }
        if (!session->get_passive_addr().has_value()) {
            return {FtpResponseCode::__425__, "Use PASV before APPE."};
        }

        const auto path = resolve_path(session, args);
        if (!path_within_root(session, path)) {
            return {FtpResponseCode::__550__, "Path is outside the FTP root."};
        }

        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
        if (ec) {
            return {FtpResponseCode::__550__, "Unable to prepare target path."};
        }

        FtpFileInfo info;
        info.mode_ = StreamMode::Receiver;
        info.origin_name_ = path.generic_string();
        info.dest_name_ = path.generic_string();
        info.append_mode_ = true;
        info.ready_ = true;

        session->get_file_manager()->reset();
        session->get_file_manager()->add_file(info);
        auto *file = session->get_file_manager()->get_next_file();
        if (!file || !session->set_work_file(file)) {
            session->clear_passive_addr();
            return {FtpResponseCode::__425__, "Passive data connection is not ready."};
        }

        return {FtpResponseCode::__150__, "Opening binary mode data connection for append."};
    }

    CommandType CommandAppe::get_command_type() { return CommandType::cmd_appe; }
    std::string CommandAppe::get_command_name() { return "APPE"; }
}
#include <filesystem>
