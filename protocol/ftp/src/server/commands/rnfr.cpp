#include "server/commands/rnfr.h"
#include "common/response_code.h"
#include "common/session.h"
#include "server/command_support.h"
#include <filesystem>
namespace yuan::net::ftp { REGISTER_COMMAND_IMPL(CommandRnfr); FtpCommandResponse CommandRnfr::execute(FtpSession *session, const std::string &args) { FtpCommandResponse denied{FtpResponseCode::invalid, ""}; if (!ensure_login(session, denied)) { return denied; } if (args.empty()) { return {FtpResponseCode::__501__, "Source path is required."}; } auto path = resolve_path(session, args); if (!path_within_root(session, path) || !std::filesystem::exists(path)) { return {FtpResponseCode::__550__, "Source path is not available."}; } session->set_item_value<std::string>("rename_from", path.generic_string()); return {FtpResponseCode::__350__, "Requested file action pending further information."}; } CommandType CommandRnfr::get_command_type() { return CommandType::cmd_rnfr; } std::string CommandRnfr::get_comand_name() { return "RNFR"; } }
