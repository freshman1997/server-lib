#include "server/commands/site.h"
#include "common/response_code.h"
namespace yuan::net::ftp { REGISTER_COMMAND_IMPL(CommandSite); FtpCommandResponse CommandSite::execute(FtpSession *session, const std::string &args) { (void)session; (void)args; return {FtpResponseCode::__502__, "SITE extensions are not implemented."}; } CommandType CommandSite::get_command_type() { return CommandType::cmd_site; } std::string CommandSite::get_comand_name() { return "SITE"; } }
