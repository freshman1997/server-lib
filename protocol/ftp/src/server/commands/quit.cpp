#include "server/commands/quit.h"
#include "common/response_code.h"
namespace yuan::net::ftp { REGISTER_COMMAND_IMPL(CommandQuit); FtpCommandResponse CommandQuit::execute(FtpSession *session, const std::string &args) { (void)session; (void)args; return {FtpResponseCode::__221__, "Goodbye.", true}; } CommandType CommandQuit::get_command_type() { return CommandType::cmd_abort; } std::string CommandQuit::get_comand_name() { return "QUIT"; } }
