#include "server/commands/pass.h"
#include "common/response_code.h"
#include "common/session.h"
#include "server/context.h"
namespace yuan::net::ftp
{
    FtpCommandResponse CommandPass::execute(FtpSession *session, const std::string &args)
    {
        if (!session) {
            return {FtpResponseCode::__503__, "Session is invalid."};
        }
        if (args.empty()) {
            return {FtpResponseCode::__501__, "Password is required."};
        }
        if (session->get_username().empty()) {
            return {FtpResponseCode::__503__, "Send USER before PASS."};
        }

        if (ServerContext::get_instance()->is_auth_required() &&
            !ServerContext::get_instance()->verify_user(session->get_username(), args)) {
            return {FtpResponseCode::__530__, "Login incorrect."};
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
