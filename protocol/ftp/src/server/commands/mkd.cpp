#include "server/commands/mkd.h"
#include "common/response_code.h"
#include "common/session.h"
#include "server/command_support.h"
#include <filesystem>
namespace yuan::net::ftp { REGISTER_COMMAND_IMPL(CommandMkd); FtpCommandResponse CommandMkd::execute(FtpSession *session, const std::string &args) { FtpCommandResponse denied{FtpResponseCode::invalid, ""}; if (!ensure_login(session, denied)) { return denied; } if (args.empty()) { return {FtpResponseCode::__501__, "Directory name is required."}; } auto path = resolve_path(session, args); if (!path_within_root(session, path)) { return {FtpResponseCode::__550__, "Directory path is not allowed."}; } std::error_code ec; std::filesystem::create_directories(path, ec); if (ec) { return {FtpResponseCode::__550__, "Cannot create directory."}; } return {FtpResponseCode::__257__, '"' + to_virtual_path(session, path) + '"' + " created."}; } CommandType CommandMkd::get_command_type() { return CommandType::cmd_mkd; } std::string CommandMkd::get_comand_name() { return "MKD"; } }
