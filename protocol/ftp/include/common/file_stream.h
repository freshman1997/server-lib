#ifndef NET_FTP_COMMON_FILE_STREAM_H
#define NET_FTP_COMMON_FILE_STREAM_H
#include "net/handler/connection_handler.h"
#include "net/socket/inet_address.h"

#include <string>
#include <unordered_map>

namespace yuan::net::ftp
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

        // Remove a child file stream session (called by FtpSession when a session wants to close).
        // Default implementation will find the session in the map, erase it and delete it.
        virtual void remove_session(FtpFileStreamSession *fs);

    public:
        bool set_work_file(FtpFileInfo *file, const std::string &ip);

    protected:
        FtpSession *session_;
        std::unordered_map<std::string, FtpFileStreamSession *> last_sessions_;
        std::unordered_map<std::string, FtpFileInfo *> pending_files_;
        std::unordered_map<std::string, FtpFileStreamSession *> file_stream_sessions_;
    };
}

#endif // NET_FTP_COMMON_FILE_STREAM_H
