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
        auto *stream_session = new FtpFileStreamSession(session_);
        const auto ip = conn->get_remote_address().get_ip();
        last_sessions_[ip] = stream_session;
        conn->set_connection_handler(stream_session);
        file_stream_sessions_[conn->get_remote_address().to_address_key()] = stream_session;
        stream_session->on_connected(conn);

        auto pending = pending_files_.find(ip);
        if (pending != pending_files_.end()) {
            auto *pending_file = pending->second;
            stream_session->set_work_file(pending_file);
            pending_files_.erase(pending);
            last_sessions_.erase(ip);
            if (pending_file && pending_file->mode_ == StreamMode::Sender) {
                stream_session->on_write(conn);
            }
        }
    }

    void FtpFileStream::on_error(Connection *conn)
    {
        std::cout << "file stream handler on_error ip=" << (conn ? conn->get_remote_address().get_ip() : std::string()) << "\n";
    }

    void FtpFileStream::on_read(Connection *conn)
    {
        std::cout << "file stream handler on_read ip=" << (conn ? conn->get_remote_address().get_ip() : std::string()) << "\n";
    }

    void FtpFileStream::on_write(Connection *conn)
    {
        std::cout << "file stream handler on_write ip=" << (conn ? conn->get_remote_address().get_ip() : std::string()) << "\n";
    }

    void FtpFileStream::on_close(Connection *conn)
    {
        std::cout << "file stream handler on_close ip=" << (conn ? conn->get_remote_address().get_ip() : std::string()) << "\n";
    }

    void FtpFileStream::quit(const InetAddress &addr)
    {
        auto it = file_stream_sessions_.find(addr.to_address_key());
        if (it != file_stream_sessions_.end()) {
            FtpFileStreamSession *sess = it->second;
            // erase mapping first to avoid reentrancy issues
            file_stream_sessions_.erase(it);
            pending_files_.erase(addr.get_ip());
            last_sessions_.erase(addr.get_ip());
            // delete the session object (its destructor won't notify session again)
            delete sess;
        }

        if (file_stream_sessions_.empty()) {
            delete this;
        }
    }

    void FtpFileStream::remove_session(FtpFileStreamSession *fs)
    {
        for (auto it = file_stream_sessions_.begin(); it != file_stream_sessions_.end(); ++it) {
            if (it->second == fs) {
                file_stream_sessions_.erase(it);
                // also try to erase by ip entries if exist
                // (we can't get the key quickly here; higher-level code usually handles pending/last maps)
                delete fs;
                break;
            }
        }

        if (file_stream_sessions_.empty()) {
            delete this;
        }
    }

    bool FtpFileStream::set_work_file(FtpFileInfo *file, const std::string &ip)
    {
        if (!file) {
            return false;
        }

        auto it = last_sessions_.find(ip);
        if (it == last_sessions_.end()) {
            pending_files_[ip] = file;
            return true;
        }

        it->second->set_work_file(file);
        last_sessions_.erase(it);
        pending_files_.erase(ip);
        return true;
    }
}
