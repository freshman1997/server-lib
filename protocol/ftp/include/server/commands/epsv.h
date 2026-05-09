#ifndef NET_FTP_SERVER_COMMAND_EPSV_H
#define NET_FTP_SERVER_COMMAND_EPSV_H

#include "../command.h"

namespace yuan::net::ftp
{
    class CommandEpsv : public Command
    {
    public:
        FtpCommandResponse execute(FtpSession *session, const std::string &args) override;
        CommandType get_command_type() override;
        std::string get_command_name() override;
    };
}

#endif
