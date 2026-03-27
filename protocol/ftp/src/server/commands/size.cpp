#include "server/commands/size.h"
#include "common/response_code.h"
#include "server/command_support.h"
#include <filesystem>
namespace yuan::net::ftp { REGISTER_COMMAND_IMPL(CommandSize); FtpCommandResponse CommandSize::execute(FtpSession *session, const std::string &args) { FtpCommandResponse denied{FtpResponseCode::invalid, ""}; if (!ensure_login(session, denied)) { return denied; } const auto path = resolve_path(session, args); if (!path_within_root(session, path) || !std::filesystem::is_regular_file(path)) { return {FtpResponseCode::__550__, "File is not available."}; } return {FtpResponseCode::__213__, std::to_string(static_cast<std::size_t>(std::filesystem::file_size(path)))}; } CommandType CommandSize::get_command_type() { return CommandType::cmd_stat; } std::string CommandSize::get_comand_name() { return "SIZE"; } }
