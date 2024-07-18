#include "net/ftp/common/file_stream.h"
#include "net/base/event/event_loop.h"
#include "net/base/socket/inet_address.h"
#include "net/base/socket/socket.h"
#include "net/ftp/common/file_stream_session.h"
#include "net/ftp/common/session.h"
#include "net/ftp/handler/ftp_app.h"
#include <cassert>
#include <iostream>

namespace net::ftp 
{
    FtpFileStream::FtpFileStream(FtpSession *session) : session_(session)
    {

    }

    FtpFileStream::~FtpFileStream()
    {
        if (session_) {
            session_ = nullptr;
        }
    }

    void FtpFileStream::on_connected(Connection *conn)
    {
        std::cout << "data stream connected, ip: " << conn->get_remote_address().get_ip() << '\n';
        assert(session_);
        FtpFileStreamSession *session = new FtpFileStreamSession(session_);
        last_sessions_[conn->get_remote_address().get_ip()] = session;
        // 重置 handler
        conn->set_connection_handler(session);
        file_stream_sessions_[conn->get_remote_address()] = session;
        session->on_connected(conn);
    }

    void FtpFileStream::on_error(Connection *conn)
    {
        assert(false);
    }

    void FtpFileStream::on_read(Connection *conn)
    {
        assert(false);
    }

    void FtpFileStream::on_write(Connection *conn)
    {
        assert(false);
    }

    void FtpFileStream::on_close(Connection *conn)
    {
        assert(false);
    }

    void FtpFileStream::quit(const InetAddress &addr)
    {
        file_stream_sessions_.erase(addr);
        if (file_stream_sessions_.empty()) {
            delete this;
        }
    }

    bool FtpFileStream::set_work_file(FtpFileInfo *file, const std::string &ip)
    {
        if (!last_sessions_.empty() || file_stream_sessions_.empty()) {
            return false;
        }

        auto it = last_sessions_.find(ip);
        if (it == last_sessions_.end()) {
            return false;
        }

        it->second->set_work_file(file);
        last_sessions_.erase(it);
        
        return true;
    }
}