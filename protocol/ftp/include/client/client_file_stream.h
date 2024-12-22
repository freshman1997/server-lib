#ifndef __NET_FTP_CLIENT_CLIENT_FILE_STREAM_H__
#define __NET_FTP_CLIENT_CLIENT_FILE_STREAM_H__
#include "common/file_stream.h"

namespace yuan::net::ftp 
{
    class FtpApp;

    class ClientFtpFileStream : public FtpFileStream
    {
    public:
        ClientFtpFileStream(FtpSession *session);
        ~ClientFtpFileStream();

    public:
        virtual bool start(const InetAddress &addr);

        virtual void quit(const InetAddress &addr);
    };
}

#endif