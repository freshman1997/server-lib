#include "server/commands/rest.h"
#include "common/response_code.h"
#include "common/session.h"
namespace yuan::net::ftp { REGISTER_COMMAND_IMPL(CommandRest); FtpCommandResponse CommandRest::execute(FtpSession *session, const std::string &args) { if (args.empty()) { return {FtpResponseCode::__501__, "Restart offset is required."}; } try { session->set_item_value<int32_t>("restart_offset", static_cast<int32_t>(std::stoll(args))); } catch (...) { return {FtpResponseCode::__501__, "Invalid restart offset."}; } return {FtpResponseCode::__350__, "Restart position accepted."}; } CommandType CommandRest::get_command_type() { return CommandType::cmd_retr; } std::string CommandRest::get_comand_name() { return "REST"; } }
