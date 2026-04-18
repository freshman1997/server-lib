#include "server/commands/rest.h"
#include "common/response_code.h"
#include "common/session.h"
#include "server/command_support.h"

namespace yuan::net::ftp
{
    FtpCommandResponse CommandRest::execute(FtpSession *session, const std::string &args)
    {
        FtpCommandResponse denied{FtpResponseCode::invalid, ""};
        if (!ensure_login(session, denied)) {
            return denied;
        }
        if (args.empty()) {
            return {FtpResponseCode::__501__, "Restart offset is required."};
        }
        try {
            session->set_item_value<int32_t>("restart_offset", std::stoi(args));
        } catch (...) {
            return {FtpResponseCode::__501__, "Restart offset is invalid."};
        }
        return {FtpResponseCode::__350__, "Restart position accepted."};
    }

    CommandType CommandRest::get_command_type() { return CommandType::cmd_rest; }
    std::string CommandRest::get_command_name() { return "REST"; }
}
#include "common/session.h"
