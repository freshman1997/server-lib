#ifndef __NET_FTP_SERVER_SERVER_PARSER_H__
#define __NET_FTP_SERVER_SERVER_PARSER_H__
#include <string_view>
#include <vector>

#include "buffer/buffer.h"
#include "common/def.h"

namespace yuan::net::ftp
{
    class FtpCommandParser
    {
    public:
        FtpCommandParser();
        ~FtpCommandParser();
        std::vector<FtpCommand> split_cmds(const std::string_view &endWith, const std::string &splitStr);
        void set_buff(buffer::Buffer *buff);
        buffer::Buffer *get_buff();

    private:
        buffer::Buffer *buff_;
    };
}

#endif
