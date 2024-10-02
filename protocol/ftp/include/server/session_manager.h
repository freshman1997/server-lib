#ifndef __NET_FTP_SERVER_SESSION_MANAGER_H__
#define __NET_FTP_SERVER_SESSION_MANAGER_H__
#include <unordered_map>

namespace net 
{
    class Connection;
}

namespace net::ftp 
{
    class FtpSession;

    class FtpSessionManager
    {
    public:
        void add_session(Connection *conn, FtpSession *session);

        FtpSession * get_session(Connection *conn);

        void remove_session(Connection *conn);

        const std::unordered_map<Connection *, FtpSession *> & get_sessions()
        {
            return sessions_;
        }

        void clear()
        {
            sessions_.clear();
        }

    private:
        std::unordered_map<Connection *, FtpSession *> sessions_;
    };
}

#endif