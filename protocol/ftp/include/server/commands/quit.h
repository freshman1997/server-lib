#ifndef NET_FTP_SERVER_COMMAND_QUIT_H
#define NET_FTP_SERVER_COMMAND_QUIT_H
#include "../command.h"

namespace yuan::net::ftp 
{
    class CommandQuit : public Command
    {
    public:
        virtual FtpCommandResponse execute(FtpSession *session, const std::string &args);

        virtual CommandType get_command_type();

        virtual std::string get_command_name();
    };
}

#endif