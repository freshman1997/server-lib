#include "server/commands/retr.h"
#include "common/response_code.h"
#include "common/session.h"
#include "server/command_support.h"
#include <filesystem>
namespace yuan::net::ftp
{
    REGISTER_COMMAND_IMPL(CommandRetr);
    FtpCommandResponse CommandRetr::execute(FtpSession *session, const std::string &args)
    {
        FtpCommandResponse denied{FtpResponseCode::invalid, ""};
        if (!ensure_login(session, denied)) {
            return denied;
        }
        if (!session->get_passive_addr().has_value()) {
            return {FtpResponseCode::__425__, "Use PASV before RETR."};
        }
        const auto path = resolve_path(session, args);
        if (!path_within_root(session, path) || !std::filesystem::is_regular_file(path)) {
            return {FtpResponseCode::__550__, "File is not available."};
        }
        const auto total = static_cast<std::size_t>(std::filesystem::file_size(path));
        const auto offset = static_cast<std::size_t>(session->get_item_value<int32_t>("restart_offset"));
        if (offset > total) {
            return {FtpResponseCode::__501__, "Restart offset exceeds file size."};
        }
        session->get_file_manager()->reset();
        FtpFileInfo info;
        info.mode_ = StreamMode::Sender;
        info.origin_name_ = path.generic_string();
        info.dest_name_ = path.filename().generic_string();
        info.file_size_ = total;
        info.current_progress_ = offset;
        info.ready_ = true;
        session->get_file_manager()->add_file(info);
        auto *file = session->get_file_manager()->get_next_file();
        if (!file || !session->set_work_file(file)) {
            session->clear_passive_addr();
            return {FtpResponseCode::__425__, "Passive data connection is not ready."};
        }
        session->remove_item("restart_offset");
        return {FtpResponseCode::__150__, "Opening binary mode data connection for file transfer."};
    }
    CommandType CommandRetr::get_command_type() { return CommandType::cmd_retr; }
    std::string CommandRetr::get_comand_name() { return "RETR"; }
}
