#include "server/commands/pass.h"
#include "common/response_code.h"
#include "common/session.h"
namespace yuan::net::ftp
{
    FtpCommandResponse CommandPass::execute(FtpSession *session, const std::string &args)
    {
        if (args.empty()) {
            return {FtpResponseCode::__501__, "Password is required."};
        }
        session->set_password(args);
        if (!session->login()) {
            return {FtpResponseCode::__503__, "Send USER before PASS."};
        }
        return {FtpResponseCode::__230__, "Login successful."};
    }
    CommandType CommandPass::get_command_type() { return CommandType::cmd_pass; }
    std::string CommandPass::get_command_name() { return "PASS"; }
}
