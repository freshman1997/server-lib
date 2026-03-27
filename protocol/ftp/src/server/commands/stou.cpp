#include "server/commands/stou.h"
#include "common/response_code.h"
#include "common/session.h"
#include "server/commands/stor.h"
#include <chrono>
namespace yuan::net::ftp
{
    REGISTER_COMMAND_IMPL(CommandStou);
    FtpCommandResponse CommandStou::execute(FtpSession *session, const std::string &args)
    {
        const auto unique = args.empty() ? ("upload_" + std::to_string(std::chrono::system_clock::now().time_since_epoch().count()) + ".bin") : args;
        CommandStor stor;
        return stor.execute(session, unique);
    }
    CommandType CommandStou::get_command_type() { return CommandType::cmd_stou; }
    std::string CommandStou::get_comand_name() { return "STOU"; }
}
