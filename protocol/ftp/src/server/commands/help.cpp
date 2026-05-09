#include "server/commands/help.h"
#include "common/response_code.h"
namespace yuan::net::ftp
{
    FtpCommandResponse CommandHelp::execute(FtpSession *session, const std::string &args)
    {
        (void)session;
        (void)args;
        return {FtpResponseCode::__214__, "USER PASS PWD CWD CDUP PASV EPSV PORT EPRT LIST NLST RETR STOR APPE SIZE ALLO MKD RMD DELE RNFR RNTO REST TYPE MODE STRU STAT NOOP QUIT"};
    }
    CommandType CommandHelp::get_command_type() { return CommandType::cmd_help; }
    std::string CommandHelp::get_command_name() { return "HELP"; }
}
