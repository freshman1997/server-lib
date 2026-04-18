#include "server/commands/rein.h"
#include "common/response_code.h"
#include "common/session.h"
#include "server/command_support.h"

namespace yuan::net::ftp
{
    FtpCommandResponse CommandRein::execute(FtpSession *session, const std::string &args)
    {
        (void)args;
        FtpCommandResponse denied{FtpResponseCode::invalid, ""};
        if (!ensure_login(session, denied)) {
            return denied;
        }
        session->remove_item("restart_offset");
        session->remove_item("rename_from");
        session->change_cwd("/");
        session->clear_passive_addr();
        return {FtpResponseCode::__220__, "Session reinitialized."};
    }

    CommandType CommandRein::get_command_type() { return CommandType::cmd_rein; }
    std::string CommandRein::get_command_name() { return "REIN"; }
}
#include "common/session.h"
