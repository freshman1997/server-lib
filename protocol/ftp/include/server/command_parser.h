#ifndef __NET_FTP_SERVER_SERVER_PARSER_H__
#define __NET_FTP_SERVER_SERVER_PARSER_H__
#include <string>
#include <vector>

#include "common/def.h"
#include "buffer/buffer.h"

namespace net::ftp 
{
    class FtpCommandParser
    {
    public:
        FtpCommandParser();
        ~FtpCommandParser();

    public:
        std::vector<FtpCommand> split_cmds(const std::string &endWith, const std::string &splitStr);

        void set_buff(Buffer *buff);

        Buffer * get_buff();

    private:
        Buffer *buff_;
    };
}

#endif