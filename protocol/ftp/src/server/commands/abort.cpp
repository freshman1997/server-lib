#include "server/commands/abort.h"
#include "common/response_code.h"
#include "common/session.h"
#include "server/command_support.h"

namespace yuan::net::ftp
{
    FtpCommandResponse CommandAbort::execute(FtpSession *session, const std::string &args)
    {
        (void)args;
        FtpCommandResponse denied{FtpResponseCode::invalid, ""};
        if (!ensure_login(session, denied)) {
            return denied;
        }
        session->get_file_manager()->reset();
        session->clear_passive_addr();
        session->remove_item("restart_offset");
        return {FtpResponseCode::__226__, "Abort command processed."};
    }

    CommandType CommandAbort::get_command_type() { return CommandType::cmd_abort; }
    std::string CommandAbort::get_command_name() { return "ABOR"; }
}
