#include "server/commands/cdup.h"
#include "common/response_code.h"
#include "common/session.h"
#include "server/command_support.h"
#include <filesystem>
namespace yuan::net::ftp { REGISTER_COMMAND_IMPL(CommandCdup); FtpCommandResponse CommandCdup::execute(FtpSession *session, const std::string &args) { (void)args; FtpCommandResponse denied{FtpResponseCode::invalid, ""}; if (!ensure_login(session, denied)) { return denied; } auto current = resolve_path(session, ""); auto parent = current.parent_path(); if (!path_within_root(session, parent)) { parent = std::filesystem::path(session->get_root_dir()); } session->change_cwd(to_virtual_path(session, parent)); return {FtpResponseCode::__250__, "Directory changed successfully."}; } CommandType CommandCdup::get_command_type() { return CommandType::cmd_cdup; } std::string CommandCdup::get_comand_name() { return "CDUP"; } }
