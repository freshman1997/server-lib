#include "common/file_stream.h"
#include "event/event_loop.h"
#include "net/socket/inet_address.h"
#include "net/socket/socket.h"
#include "common/file_stream_session.h"
#include "common/session.h"
#include "handler/ftp_app.h"
#include <cassert>
#include <iostream>

namespace yuan::net::ftp 
{
    FtpFileStream::FtpFileStream(FtpSession *session) : session_(session)
    {

    }

    FtpFileStream::~FtpFileStream()
    {
        if (session_) {
            session_->on_file_stream_close(this);
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
        auto it = file_stream_sessions_.find(addr);
        if (it != file_stream_sessions_.end()) {
            it->second->quit();
            file_stream_sessions_.erase(it);
        }

        if (file_stream_sessions_.empty()) {
            delete this;
        }
    }

    bool FtpFileStream::set_work_file(FtpFileInfo *file, const std::string &ip)
    {
        if (last_sessions_.empty() || file_stream_sessions_.empty()) {
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