#include "server/commands/acct.h"
#include "common/response_code.h"
namespace yuan::net::ftp { REGISTER_COMMAND_IMPL(CommandAcct); FtpCommandResponse CommandAcct::execute(FtpSession *session, const std::string &args) { (void)session; (void)args; return {FtpResponseCode::__502__, "ACCT is not implemented."}; } CommandType CommandAcct::get_command_type() { return CommandType::cmd_acct; } std::string CommandAcct::get_comand_name() { return "ACCT"; } }
