#ifndef NET_FTP_SERVER_COMMAND_PARSER_H
#define NET_FTP_SERVER_COMMAND_PARSER_H
#include <string_view>
#include <vector>

#include "buffer/byte_buffer.h"
#include "common/def.h"

namespace yuan::net::ftp
{
    class FtpCommandParser
    {
    public:
        FtpCommandParser();
        ~FtpCommandParser();
        std::vector<FtpCommand> split_cmds(const std::string_view &endWith, const std::string &splitStr);
        void set_buff(const ::yuan::buffer::ByteBuffer &buff);
        void set_buff(::yuan::buffer::ByteBuffer &&buff);

    private:
        ::yuan::buffer::ByteBuffer buff_;
    };
}

#endif // NET_FTP_SERVER_COMMAND_PARSER_H
