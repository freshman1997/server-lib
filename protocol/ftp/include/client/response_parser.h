#ifndef __NET_FTP_CLIENT_RESPONSE_PARSER_H__
#define __NET_FTP_CLIENT_RESPONSE_PARSER_H__

#include "buffer/buffer.h"
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

        void set_buff(buffer::Buffer *buff);
        std::vector<FtpClientResponse> split_responses(const std::string_view &end_with = "\r\n");

    private:
        buffer::Buffer *buff_;
    };
}

#endif
