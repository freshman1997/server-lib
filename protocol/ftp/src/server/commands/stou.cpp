#include "server/commands/stou.h"
#include "base/time.h"
#include "common/response_code.h"
#include "common/session.h"
#include "server/commands/stor.h"
namespace yuan::net::ftp
{
    FtpCommandResponse CommandStou::execute(FtpSession *session, const std::string &args)
    {
        const auto unique = args.empty() ? ("upload_" + std::to_string(base::time::system_now_us()) + ".bin") : args;
        CommandStor stor;
        return stor.execute(session, unique);
    }
    CommandType CommandStou::get_command_type() { return CommandType::cmd_stou; }
    std::string CommandStou::get_command_name() { return "STOU"; }
}
