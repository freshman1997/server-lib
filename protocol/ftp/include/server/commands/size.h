#ifndef NET_FTP_SERVER_COMMAND_SIZE_H
#define NET_FTP_SERVER_COMMAND_SIZE_H
#include "../command.h"
namespace yuan::net::ftp { class CommandSize : public Command { public: virtual FtpCommandResponse execute(FtpSession *session, const std::string &args); virtual CommandType get_command_type(); virtual std::string get_command_name(); }; }
#endif
