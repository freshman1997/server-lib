#ifndef __NET_FTP_RESPONSE_BUIDLER_H__
#define __NET_FTP_RESPONSE_BUIDLER_H__
#include "net/base/connection/connection.h"
#include "net/ftp/response_code.h"

namespace net::ftp 
{
    class ResponseBuilder
    {
    public:
        ResponseBuilder(Connection *conn);
        
        ResponseBuilder * set_response_code(FtpResponseCode code);

        void flush();

    private:
        Connection *conn_;
        FtpResponseCode code_;
        
    };
}

#endif