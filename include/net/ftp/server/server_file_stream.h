#ifndef __NET_FTP_SERVER_SERVER_FILE_STREAM_H__
#define __NET_FTP_SERVER_SERVER_FILE_STREAM_H__
#include "../common/file_stream.h"

namespace net 
{
    class Acceptor;
}

namespace net::ftp 
{
    class FtpApp;

    class ServerFtpFileStream : public FtpFileStream
    {
    public:
        ServerFtpFileStream(FtpSession *session);
        ~ServerFtpFileStream();

    public:
        virtual void on_connected(Connection *conn);

        virtual bool start(const InetAddress &addr);

        virtual void quit(const InetAddress &addr);

    private:
        Acceptor *acceptor_;
    };
}

#endif