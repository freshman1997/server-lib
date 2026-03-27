#include "server/commands/allo.h"
#include "common/response_code.h"
#include "common/session.h"
namespace yuan::net::ftp { REGISTER_COMMAND_IMPL(CommandAllo); FtpCommandResponse CommandAllo::execute(FtpSession *session, const std::string &args) { if (args.empty()) { return {FtpResponseCode::__501__, "Allocation size is required."}; } try { session->set_item_value<int32_t>("upload_file_size", static_cast<int32_t>(std::stoll(args))); } catch (...) { return {FtpResponseCode::__501__, "Invalid allocation size."}; } return {FtpResponseCode::__200__, "Allocation recorded."}; } CommandType CommandAllo::get_command_type() { return CommandType::cmd_allo; } std::string CommandAllo::get_comand_name() { return "ALLO"; } }
