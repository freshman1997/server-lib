#include "server/session_manager.h"

namespace yuan::net::ftp 
{
    void FtpSessionManager::add_session(Connection *conn, FtpSession *session)
    {
        if (conn) {
            sessions_[conn] = session;
        }
    }

    FtpSession * FtpSessionManager::get_session(Connection *conn)
    {
        auto it = sessions_.find(conn);
        return it == sessions_.end() ? nullptr : it->second;
    }

    void FtpSessionManager::remove_session(Connection *conn)
    {
        if (conn) {
            sessions_.erase(conn);
        }
    }
}