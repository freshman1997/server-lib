#include "server/commands/stru.h"
#include "common/response_code.h"
namespace yuan::net::ftp { REGISTER_COMMAND_IMPL(CommandStru); FtpCommandResponse CommandStru::execute(FtpSession *session, const std::string &args) { (void)session; return args == "F" ? FtpCommandResponse{FtpResponseCode::__200__, "File structure set."} : FtpCommandResponse{FtpResponseCode::__504__, "Only STRU F is supported."}; } CommandType CommandStru::get_command_type() { return CommandType::cmd_help; } std::string CommandStru::get_comand_name() { return "STRU"; } }
