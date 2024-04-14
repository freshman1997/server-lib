#ifndef __NET_FTP_FILE_STREAM_H__
#define __NET_FTP_FILE_STREAM_H__
#include "net/base/handler/connection_handler.h"

namespace net::ftp 
{
    class FtpFileStream : public ConnectionHandler
    {
    public:
        FtpFileStream();
        virtual ~FtpFileStream();

    public:
        virtual void on_connected(Connection *conn);

        virtual void on_error(Connection *conn);

        virtual void on_read(Connection *conn);

        virtual void on_write(Connection *conn);

        virtual void on_close(Connection *conn);

    protected:
        
    };

    class FtpServerFileStream : public FtpFileStream
    {

    };

    class FtpClientFileStream : public FtpFileStream
    {

    };
}

#endif