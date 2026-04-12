#ifndef __NET_FTP_CLIENT_RESPONSE_PARSER_H__
#define __NET_FTP_CLIENT_RESPONSE_PARSER_H__

#include "buffer/byte_buffer.h"
#include "client/context.h"

#include <string_view>
#include <vector>

namespace yuan::net::ftp
{
    class FtpResponseParser
    {
    public:
        FtpResponseParser();
        ~FtpResponseParser();

        void set_buff(const ::yuan::buffer::ByteBuffer &buff);
        void set_buff(::yuan::buffer::ByteBuffer &&buff);
        std::vector<FtpClientResponse> split_responses(const std::string_view &end_with = "\r\n");

    private:
        ::yuan::buffer::ByteBuffer buff_;
    };
}

#endif
