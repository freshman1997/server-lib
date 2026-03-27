#ifndef __NET_FTP_SERVER_SESSION_MANAGER_H__
#define __NET_FTP_SERVER_SESSION_MANAGER_H__

#include <unordered_map>
#include <memory>

namespace yuan::net
{
    class Connection;
}

namespace yuan::net::ftp
{
    class FtpSession;

    class FtpSessionManager
    {
    public:
        void add_session(Connection *conn, std::shared_ptr<FtpSession> session)
        {
            sessions_[conn] = session;
        }

        std::shared_ptr<FtpSession> get_session(Connection *conn)
        {
            auto it = sessions_.find(conn);
            return it != sessions_.end() ? it->second : nullptr;
        }

        void remove_session(Connection *conn)
        {
            sessions_.erase(conn);  // shared_ptr 自动管理生命周期
        }

        void remove_by_session(FtpSession *session)
        {
            for (auto it = sessions_.begin(); it != sessions_.end(); ++it) {
                if (it->second.get() == session) {
                    sessions_.erase(it);
                    return;
                }
            }
        }

        const std::unordered_map<Connection *, std::shared_ptr<FtpSession>> & get_sessions() const
        {
            return sessions_;
        }

        void clear()
        {
            sessions_.clear();  // 自动删除所有 session
        }

    private:
        std::unordered_map<Connection *, std::shared_ptr<FtpSession>> sessions_;
    };
}

#endif