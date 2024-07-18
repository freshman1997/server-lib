#ifndef __NET_FTP_SERVER_COMMAND_APPE_H__
#define __NET_FTP_SERVER_COMMAND_APPE_H__
#include "../command.h"

namespace net::ftp 
{
    class CommandAppe : public Command
    {
    public:
        virtual FtpCommandResponse execute(FtpSession *session, const std::string &args);

        virtual CommandType get_command_type();

        virtual std::string get_comand_name();
    };
}

#endif