#ifndef __NET_FTP_FILE_STREAM_H__
#define __NET_FTP_FILE_STREAM_H__
#include <fstream>

#include "net/base/handler/connection_handler.h"

namespace net::ftp 
{
    class FtpSession;

    enum class StreamMode : char
    {
        Sender,
        Receiver
    };
    
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

    public:
        void using_mode(StreamMode mode)
        {
            mode_ = mode;
        }

        void open_file_stream(const std::string &filepath);

    protected:
        bool connected_;
        StreamMode mode_;
        FtpSession *session_;
        std::fstream *file_stream_;
    };
}

#endif