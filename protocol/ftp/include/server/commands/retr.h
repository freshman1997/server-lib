#ifndef NET_FTP_SERVER_COMMAND_RETR_H
#define NET_FTP_SERVER_COMMAND_RETR_H
#include "../command.h"

namespace yuan::net::ftp 
{
    class CommandRetr : public Command
    {
    public:
        virtual FtpCommandResponse execute(FtpSession *session, const std::string &args);

        virtual CommandType get_command_type();

        virtual std::string get_command_name();
    };
}

#endif