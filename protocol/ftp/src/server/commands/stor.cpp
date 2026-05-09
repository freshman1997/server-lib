#include "server/commands/stor.h"
#include "common/response_code.h"
#include "common/session.h"
#include "server/command_support.h"
#include <filesystem>

namespace yuan::net::ftp
{
    FtpCommandResponse CommandStor::execute(FtpSession *session, const std::string &args)
    {
        FtpCommandResponse denied{FtpResponseCode::invalid, ""};
        if (!ensure_login(session, denied)) {
            return denied;
        }
        if (!session->get_passive_addr().has_value()) {
            if (!session->get_active_addr().has_value()) {
                return {FtpResponseCode::__425__, "Use PASV or PORT before STOR."};
            }
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
        info.file_size_ = static_cast<std::size_t>(session->get_item_value<int32_t>("upload_file_size"));
        info.ready_ = true;

        session->get_file_manager()->reset();
        session->get_file_manager()->add_file(info);
        auto *file = session->get_file_manager()->get_next_file();
        if (!file || !session->set_work_file(file)) {
            session->clear_passive_addr();
            return {FtpResponseCode::__425__, "Passive data connection is not ready."};
        }

        session->remove_item("restart_offset");
        session->remove_item("upload_file_size");
        return {FtpResponseCode::__150__, "Opening binary mode data connection for upload."};
    }

    CommandType CommandStor::get_command_type() { return CommandType::cmd_stor; }
    std::string CommandStor::get_command_name() { return "STOR"; }
}
