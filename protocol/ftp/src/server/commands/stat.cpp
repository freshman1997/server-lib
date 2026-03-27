#include "server/commands/stat.h"
#include "common/response_code.h"
#include "common/session.h"
#include "server/command_support.h"
#include <filesystem>
namespace yuan::net::ftp { REGISTER_COMMAND_IMPL(CommandStat); FtpCommandResponse CommandStat::execute(FtpSession *session, const std::string &args) { FtpCommandResponse denied{FtpResponseCode::invalid, ""}; if (!ensure_login(session, denied)) { return denied; } if (args.empty()) { return {FtpResponseCode::__211__, "cwd=" + session->get_cwd() + "; passive=" + (session->get_passive_addr().has_value() ? std::string{"ready"} : std::string{"idle"})}; } auto path = resolve_path(session, args); if (!path_within_root(session, path) || !std::filesystem::exists(path)) { return {FtpResponseCode::__550__, "Path is not available."}; } if (std::filesystem::is_regular_file(path)) { return {FtpResponseCode::__213__, std::to_string(static_cast<std::size_t>(std::filesystem::file_size(path)))}; } return {FtpResponseCode::__212__, "Directory status available."}; } CommandType CommandStat::get_command_type() { return CommandType::cmd_stat; } std::string CommandStat::get_comand_name() { return "STAT"; } }
