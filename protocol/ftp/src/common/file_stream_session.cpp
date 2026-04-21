#include "common/file_stream_session.h"
#include "base/time.h"
#include "buffer/byte_buffer.h"
#include "common/session.h"
#include "net/channel/channel.h"
#include "net/connection/stream_transport.h"
#include "handler/ftp_app.h"
#include "net/runtime/network_runtime.h"
#include "logger.h"

#include <cassert>

namespace yuan::net::ftp
{
    FtpFileStreamSession::FtpFileStreamSession(FtpSession * session)
    {
        state_ = FileStreamState::init;
        current_file_info_ = nullptr;
        conn_timer_ = nullptr;
        conn_owner_.reset();
        conn_ = nullptr;
        remote_addr_ = InetAddress{};
        session_ = session;
        auto *runtime = session_->get_app()->get_runtime();
        assert(runtime);
        conn_timer_ = runtime->schedule_periodic(2 * 1000, 10 * 1000, [this]() { on_timer(nullptr); }, -1);
        last_active_time_ = 0;
        write_buff_size_ = default_write_buff_size;
    }

    FtpFileStreamSession::~FtpFileStreamSession()
    {
        if (conn_timer_) {
            conn_timer_->cancel();
            conn_timer_ = nullptr;
        }
        // Do not notify session here: session notification is performed by quit()
        // to avoid recursive removal/delete cycles.
        session_ = nullptr;
        if (auto c = conn_owner_.lock()) {
            c->close();
        }
    }

    void FtpFileStreamSession::on_connected(const std::shared_ptr<Connection> &conn)
    {
        if (conn) {
            on_connected(*conn);
        }
    }

    void FtpFileStreamSession::on_connected(Connection & conn)
    {
        if (conn_) {
            conn.close();
            return;
        }
        state_ = FileStreamState::connected;
        conn_owner_ = conn.shared_from_this();
        conn_ = &conn;
        remote_addr_ = conn.get_remote_address();
        LOG_DEBUG("file session connected remote={}:{}", remote_addr_.get_ip(), remote_addr_.get_port());
        session_->on_opened(this);
        last_active_time_ = base::time::now();
    }

    void FtpFileStreamSession::on_error(const std::shared_ptr<Connection> &conn)
    {
        if (conn) {
            on_error(*conn);
        }
    }

    void FtpFileStreamSession::on_error(Connection & conn)
    {
        (void)conn;
        state_ = FileStreamState::connection_error;
        session_->on_error(this);
        // Defer close to avoid use-after-free
        if (auto c = conn_owner_.lock()) {
            conn_owner_.reset();
            conn_ = nullptr;
            session_->get_app()->get_runtime()->dispatch([c]() { c->close(); });
        }
    }

    void FtpFileStreamSession::on_read(const std::shared_ptr<Connection> &conn)
    {
        if (conn) {
            on_read(*conn);
        }
    }

    void FtpFileStreamSession::on_read(Connection & conn)
    {
        if (!current_file_info_ || current_file_info_->mode_ != StreamMode::Receiver || !current_file_info_->ready_) {
            LOG_DEBUG("file session read skipped");
            return;
        }
        state_ = FileStreamState::processing;
        auto buff = conn.take_input_byte_buffer();
        LOG_DEBUG("file session read bytes={} dest={}", buff.readable_bytes(), current_file_info_->dest_name_);
        int ret = current_file_info_->write_file(buff);
        if (ret < 0) {
            state_ = FileStreamState::file_error;
            session_->on_error(this);
            // Defer close to avoid use-after-free (see on_read completion path)
            if (auto c = conn_owner_.lock()) {
                conn_owner_.reset();
                conn_ = nullptr;
                session_->get_app()->get_runtime()->dispatch([c]() { c->close(); });
            }
            return;
        }
        if (current_file_info_->is_completed()) {
            state_ = FileStreamState::idle;
            current_file_info_->ready_ = false;
            session_->on_completed(this);
            current_file_info_ = nullptr;
            // Defer close to avoid use-after-free: on_read is called from
            // TcpConnection::on_read_event(), and conn_->close() synchronously
            // deletes the TcpConnection via do_close()->delete this, causing UB
            // when control returns to on_read_event().
            if (auto c = conn_owner_.lock()) {
                conn_owner_.reset();
                conn_ = nullptr;
                session_->get_app()->get_runtime()->dispatch([c]() { c->close(); });
            }
            return;
        }
        last_active_time_ = base::time::now();
    }

    void FtpFileStreamSession::on_write(const std::shared_ptr<Connection> &conn)
    {
        if (conn) {
            on_write(*conn);
        }
    }

    void FtpFileStreamSession::on_write(Connection & conn)
    {
        if (!current_file_info_ || current_file_info_->mode_ != StreamMode::Sender || !current_file_info_->ready_) {
            LOG_DEBUG("file session write skipped");
            return;
        }
        state_ = FileStreamState::processing;
        yuan::buffer::ByteBuffer buff(write_buff_size_);
        int ret = current_file_info_->read_file(write_buff_size_, buff);
        LOG_DEBUG("file session write ret={} source={} progress={}/{}", ret, current_file_info_->origin_name_, current_file_info_->current_progress_, current_file_info_->file_size_);
        if (ret < 0) {
            state_ = FileStreamState::file_error;
            session_->on_error(this);
            // Defer close to avoid use-after-free (see on_read above)
            if (auto c = conn_owner_.lock()) {
                conn_owner_.reset();
                conn_ = nullptr;
                session_->get_app()->get_runtime()->dispatch([c]() { c->close(); });
            }
            return;
        }
        if (buff.readable_bytes() > 0) {
            conn.write(buff);
        }
        if (current_file_info_->is_completed()) {
            state_ = FileStreamState::idle;
            current_file_info_->ready_ = false;
            conn.flush();
            session_->on_completed(this);
            current_file_info_ = nullptr;
            // Defer close to avoid use-after-free (see on_read above)
            if (auto c = conn_owner_.lock()) {
                conn_owner_.reset();
                conn_ = nullptr;
                session_->get_app()->get_runtime()->dispatch([c]() { c->close(); });
            }
            return;
        }
        last_active_time_ = base::time::now();
        conn.flush();
    }

    void FtpFileStreamSession::on_close(const std::shared_ptr<Connection> &conn)
    {
        if (conn) {
            on_close(*conn);
        }
    }

    void FtpFileStreamSession::on_close(Connection & conn)
    {
        (void)conn;
        LOG_DEBUG("file session close remote={}:{}", remote_addr_.get_ip(), remote_addr_.get_port());
        if (state_ == FileStreamState::disconnected) {
            return;
        }
        if (current_file_info_ && current_file_info_->mode_ == StreamMode::Receiver && current_file_info_->file_size_ == 0) {
            current_file_info_->state_ = FileState::processed;
            current_file_info_->ready_ = false;
            session_->on_completed(this);
            current_file_info_ = nullptr;
        }
        conn_owner_.reset();
        conn_ = nullptr;
        quit();
    }

    void FtpFileStreamSession::on_timer(timer::Timer * timer)
    {
        (void)timer;
        if (state_ == FileStreamState::idle && base::time::now() - last_active_time_ >= default_session_idle_timeout) {
            state_ = FileStreamState::idle_timeout;
            session_->on_idle_timeout(this);
            quit();
        } else if (state_ == FileStreamState::connecting) {
            state_ = FileStreamState::connect_timeout;
            session_->on_connect_timeout(this);
            quit();
        } else {
            state_ = FileStreamState::idle;
        }
    }

    Connection *FtpFileStreamSession::get_connection()
    {
        return conn_;
    }
    const InetAddress &FtpFileStreamSession::get_remote_address() const
    {
        return remote_addr_;
    }
    std::size_t FtpFileStreamSession::get_write_buff_size()
    {
        return write_buff_size_;
    }
    void FtpFileStreamSession::set_write_buff_size(std::size_t sz)
    {
        write_buff_size_ = sz;
    }
    void FtpFileStreamSession::set_work_file(FtpFileInfo * info)
    {
        current_file_info_ = info;
        LOG_DEBUG("set work file mode={} origin={} dest={}", info ? static_cast<int>(info->mode_) : -1,
                  info ? info->origin_name_ : std::string(), info ? info->dest_name_ : std::string());
        if (conn_ && info) {
            auto *stream = dynamic_cast<StreamTransport *>(conn_);
            auto *channel = stream ? stream->stream_channel() : nullptr;
            if (!channel) {
                return;
            }
            if (info->mode_ == StreamMode::Receiver) {
                channel->disable_write();
                channel->enable_read();
            } else {
                channel->disable_read();
                channel->enable_write();
            }
            session_->get_app()->get_runtime()->update_channel(channel);
        }
    }
    FtpFileInfo *FtpFileStreamSession::get_work_file()
    {
        return current_file_info_;
    }
    void FtpFileStreamSession::quit()
    {
        if (state_ == FileStreamState::disconnected) {
            return;
        }
        state_ = FileStreamState::disconnected;
        // stop timer and close connection; ownership removal is delegated to FtpFileStream via session->on_closed
        if (conn_timer_) {
            conn_timer_->cancel();
            conn_timer_ = nullptr;
        }
        if (auto c = conn_owner_.lock()) {
            c->close();
        }
        if (session_) {
            // let the session handle removing this file stream from the owning FtpFileStream
            auto *s = session_;
            session_ = nullptr;
            s->on_closed(this);
        }
        // do not delete this here; the owner (FtpFileStream::remove_session or quit) will delete
    }
    FileStreamState FtpFileStreamSession::get_state()
    {
        return state_;
    }
}
