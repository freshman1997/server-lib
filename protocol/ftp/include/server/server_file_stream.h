#ifndef NET_FTP_SERVER_SERVER_FILE_STREAM_H
#define NET_FTP_SERVER_SERVER_FILE_STREAM_H
#include "common/file_stream.h"

namespace yuan::net 
{
    class StreamAcceptor;
}

namespace yuan::net::ftp 
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
        std::unique_ptr<StreamAcceptor> acceptor_;
    };
}

#endif
