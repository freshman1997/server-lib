#include "common/response_code.h"
#include "common/session.h"
#include "server/commands/user.h"
namespace yuan::net::ftp { REGISTER_COMMAND_IMPL(CommandUser); FtpCommandResponse CommandUser::execute(FtpSession *session, const std::string &args) { if (args.empty()) { return {FtpResponseCode::__501__, "Username is required."}; } session->set_username(args); return {FtpResponseCode::__331__, "User name okay, need password."}; } CommandType CommandUser::get_command_type() { return CommandType::cmd_user; } std::string CommandUser::get_comand_name() { return "USER"; } }
