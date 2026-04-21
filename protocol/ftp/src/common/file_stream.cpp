#include "common/file_stream.h"
#include "event/event_loop.h"
#include "net/socket/inet_address.h"
#include "net/socket/socket.h"
#include "common/file_stream_session.h"
#include "common/session.h"
#include "handler/ftp_app.h"
#include <cassert>
#include "logger.h"

namespace yuan::net::ftp
{
    FtpFileStream::FtpFileStream(FtpSession * session)
        : session_(session)
    {
    }

    FtpFileStream::~FtpFileStream()
    {
        if (session_) {
            session_->on_file_stream_close(this);
            session_ = nullptr;
        }
    }

    void FtpFileStream::on_connected(const std::shared_ptr<Connection> &conn)
    {
        if (conn) {
            on_connected(*conn);
        }
    }

    void FtpFileStream::on_connected(Connection & conn)
    {
        LOG_DEBUG("data stream connected ip: {}", conn.get_remote_address().get_ip());
        assert(session_);
        auto stream_session = std::make_shared<FtpFileStreamSession>(session_);
        const auto ip = conn.get_remote_address().get_ip();
        const auto addr_key = conn.get_remote_address().to_address_key();

        if (last_sessions_.find(ip) != last_sessions_.end()) {
            LOG_WARN("file stream last_sessions_ collision for ip={}, overwriting", ip);
        }
        last_sessions_[ip] = stream_session;
        conn.set_connection_handler(make_aliasing_handler(stream_session, &*stream_session));
        file_stream_sessions_[addr_key] = stream_session;
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

    void FtpFileStream::on_error(const std::shared_ptr<Connection> &conn)
    {
        if (conn) {
            on_error(*conn);
        }
    }

    void FtpFileStream::on_error(Connection & conn)
    {
        LOG_WARN("file stream on_error ip={}", conn.get_remote_address().get_ip());
    }

    void FtpFileStream::on_read(const std::shared_ptr<Connection> &conn)
    {
        if (conn) {
            on_read(*conn);
        }
    }

    void FtpFileStream::on_read(Connection & conn)
    {
        LOG_DEBUG("file stream on_read ip={}", conn.get_remote_address().get_ip());
    }

    void FtpFileStream::on_write(const std::shared_ptr<Connection> &conn)
    {
        if (conn) {
            on_write(*conn);
        }
    }

    void FtpFileStream::on_write(Connection & conn)
    {
        LOG_DEBUG("file stream on_write ip={}", conn.get_remote_address().get_ip());
    }

    void FtpFileStream::on_close(const std::shared_ptr<Connection> &conn)
    {
        if (conn) {
            on_close(*conn);
        }
    }

    void FtpFileStream::on_close(Connection & conn)
    {
        LOG_DEBUG("file stream on_close ip={}", conn.get_remote_address().get_ip());
    }

    void FtpFileStream::quit(const InetAddress & addr)
    {
        auto it = file_stream_sessions_.find(addr.to_address_key());
        if (it != file_stream_sessions_.end()) {
            // erase mapping first to avoid reentrancy issues
            file_stream_sessions_.erase(it);
            pending_files_.erase(addr.get_ip());
            last_sessions_.erase(addr.get_ip());
        }
    }

    void FtpFileStream::remove_session(FtpFileStreamSession * fs)
    {
        for (auto it = file_stream_sessions_.begin(); it != file_stream_sessions_.end(); ++it) {
            if (&*it->second == fs) {
                file_stream_sessions_.erase(it);
                break;
            }
        }

        for (auto it = last_sessions_.begin(); it != last_sessions_.end(); ++it) {
            if (&*it->second == fs) {
                last_sessions_.erase(it);
                break;
            }
        }

        if (fs->get_work_file()) {
            for (auto it = pending_files_.begin(); it != pending_files_.end(); ++it) {
                if (it->second == fs->get_work_file()) {
                    pending_files_.erase(it);
                    break;
                }
            }
        }

    }

    bool FtpFileStream::set_work_file(FtpFileInfo * file, const std::string & ip)
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
