#ifndef _NET_FTP_CLIENT_FILE_STREAM_H__
#define _NET_FTP_CLIENT_FILE_STREAM_H__
#include "../../base/handler/connection_handler.h"
#include "../../base/socket/inet_address.h"

#include <string>
#include <unordered_map>

namespace net::ftp 
{
    class FtpApp;
    class FtpSession;
    class FtpFileStreamSession;
    struct FtpFileInfo;

    class FtpFileStream : public ConnectionHandler
    {
    public:
        FtpFileStream(FtpSession *session);
        virtual ~FtpFileStream();

    public:
        virtual void on_connected(Connection *conn);

        virtual void on_error(Connection *conn);

        virtual void on_read(Connection *conn);

        virtual void on_write(Connection *conn);

        virtual void on_close(Connection *conn);

    public:
        virtual bool start(const InetAddress &addr) = 0;

        virtual void quit(const InetAddress &addr);

    public:
        bool set_work_file(FtpFileInfo *file, const std::string &ip);

    protected:
        FtpSession *session_;
        std::unordered_map<std::string, FtpFileStreamSession *> last_sessions_;
        std::unordered_map<InetAddress, FtpFileStreamSession *> file_stream_sessions_;
    };
}

#endif