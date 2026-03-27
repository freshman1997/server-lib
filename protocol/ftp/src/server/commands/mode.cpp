#include "server/commands/mode.h"
#include "common/response_code.h"
namespace yuan::net::ftp { REGISTER_COMMAND_IMPL(CommandMode); FtpCommandResponse CommandMode::execute(FtpSession *session, const std::string &args) { (void)session; return args == "S" ? FtpCommandResponse{FtpResponseCode::__200__, "Transfer mode set to Stream."} : FtpCommandResponse{FtpResponseCode::__504__, "Only MODE S is supported."}; } CommandType CommandMode::get_command_type() { return CommandType::cmd_help; } std::string CommandMode::get_comand_name() { return "MODE"; } }
