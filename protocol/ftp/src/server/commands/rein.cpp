#include "server/commands/rein.h"
#include "common/response_code.h"
#include "common/session.h"
namespace yuan::net::ftp { REGISTER_COMMAND_IMPL(CommandRein); FtpCommandResponse CommandRein::execute(FtpSession *session, const std::string &args) { (void)args; session->set_username(""); session->set_password(""); session->remove_item("rename_from"); session->remove_item("restart_offset"); session->remove_item("upload_file_size"); return {FtpResponseCode::__220__, "Session reset."}; } CommandType CommandRein::get_command_type() { return CommandType::cmd_rein; } std::string CommandRein::get_comand_name() { return "REIN"; } }
