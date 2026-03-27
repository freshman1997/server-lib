#include "server/commands/type.h"
#include "common/response_code.h"
namespace yuan::net::ftp { REGISTER_COMMAND_IMPL(CommandTypeCmd); FtpCommandResponse CommandTypeCmd::execute(FtpSession *session, const std::string &args) { (void)session; if (args != "I" && args != "A") { return {FtpResponseCode::__504__, "Only TYPE I and TYPE A are supported."}; } return {FtpResponseCode::__200__, "Type set successfully."}; } CommandType CommandTypeCmd::get_command_type() { return CommandType::cmd_help; } std::string CommandTypeCmd::get_comand_name() { return "TYPE"; } }
