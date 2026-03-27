#include "server/commands/list.h"
#include "common/response_code.h"
#include "common/session.h"
#include "server/command_support.h"

#include <filesystem>

namespace yuan::net::ftp
{
    REGISTER_COMMAND_IMPL(CommandList);

    FtpCommandResponse CommandList::execute(FtpSession *session, const std::string &args)
    {
        FtpCommandResponse denied{FtpResponseCode::invalid, ""};
        if (!ensure_login(session, denied)) {
            return denied;
        }
        if (!session->get_passive_addr().has_value()) {
            return {FtpResponseCode::__425__, "Use PASV before LIST."};
        }

        const auto target = resolve_path(session, args);
        if (!path_within_root(session, target) || !std::filesystem::exists(target)) {
            return {FtpResponseCode::__550__, "Path is not available."};
        }

        FtpFileInfo info;
        info.mode_ = StreamMode::Sender;
        info.type_ = FileType::directionary;
        info.in_memory_ = true;
        info.memory_content_ = build_list_payload(target);
        info.file_size_ = info.memory_content_.size();
        info.ready_ = true;

        session->get_file_manager()->reset();
        session->get_file_manager()->add_file(info);
        auto *file = session->get_file_manager()->get_next_file();
        if (!file || !session->set_work_file(file)) {
            session->clear_passive_addr();
            return {FtpResponseCode::__425__, "Passive data connection is not ready."};
        }

        return {FtpResponseCode::__150__, "Opening ASCII mode data connection for file list."};
    }

    CommandType CommandList::get_command_type() { return CommandType::cmd_list; }
    std::string CommandList::get_comand_name() { return "LIST"; }
}
