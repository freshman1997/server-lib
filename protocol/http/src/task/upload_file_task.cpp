#include "task/upload_file_task.h"

#include "logger.h"
#include "net/channel/channel.h"
#include "net/connection/connection.h"
#include "net/connection/stream_transport.h"

#include <algorithm>
#include <cstring>
#include <filesystem>


#ifdef __linux__
#include <cerrno>
#include <fcntl.h>
#include <sys/sendfile.h>
#include <unistd.h>
#endif

namespace yuan::net::http
{
    HttpUploadFileTask::HttpUploadFileTask(std::function<void()> completedCb)
        : completed_callback_(std::move(completedCb))
    {
    }

    bool HttpUploadFileTask::init()
    {
        if (!attachment_info_) {
            return false;
        }

        if (attachment_info_->origin_file_name_.empty()) {
            return false;
        }

        const auto file_path = std::filesystem::path(std::filesystem::u8path(attachment_info_->origin_file_name_));
        if (file_path.is_relative()) {
            return false;
        }

        if (!std::filesystem::exists(file_path)) {
            return false;
        }

        std::error_code ec;
        const auto file_size = std::filesystem::file_size(file_path, ec);
        if (ec ||
            attachment_info_->source_offset_ > file_size ||
            attachment_info_->length_ > file_size - attachment_info_->source_offset_) {
            return false;
        }

        attachment_info_->offset_ = 0;

#ifdef __linux__
        sendfile_chunk_size_ = 256 * 1024;
        if (file_fd_ >= 0) {
            ::close(file_fd_);
            file_fd_ = -1;
        }
        file_fd_ = ::open(file_path.c_str(), O_RDONLY | O_CLOEXEC);
        if (file_fd_ < 0) {
            return false;
        }
        if (::lseek(file_fd_, static_cast<off_t>(attachment_info_->source_offset_), SEEK_SET) < 0) {
            ::close(file_fd_);
            file_fd_ = -1;
            return false;
        }
#endif

        file_stream_.open(file_path, std::ios::in | std::ios::binary);
        if (!file_stream_.is_open()) {
#ifdef __linux__
            if (file_fd_ >= 0) {
                ::close(file_fd_);
                file_fd_ = -1;
            }
#endif
            return false;
        }

        file_stream_.seekg(static_cast<std::streamoff>(attachment_info_->source_offset_), std::ios::beg);
        return file_stream_.good();
    }

    bool HttpUploadFileTask::write_to_connection(::yuan::net::Connection *conn)
    {
#ifdef __linux__
        if (!sendfile_enabled_ || !conn || !attachment_info_ || file_fd_ < 0) {
            return false;
        }

        auto *transport = dynamic_cast<::yuan::net::StreamTransport *>(conn);
        if (!transport || conn->get_ssl_handler()) {
            return false;
        }

        const auto *channel = transport->stream_channel();
        if (!channel) {
            return false;
        }

        const int socket_fd = channel->get_fd();
        if (socket_fd < 0) {
            return false;
        }

        if (attachment_info_->offset_ >= attachment_info_->length_) {
            (void)check_completed();
            return true;
        }

        const std::size_t remaining = attachment_info_->length_ - attachment_info_->offset_;
        const std::size_t to_send = (std::min)(remaining, sendfile_chunk_size_);

        const auto sent = ::sendfile(socket_fd, file_fd_, nullptr, to_send);
        if (sent > 0) {
            const auto sent_size = static_cast<std::size_t>(sent);
            attachment_info_->offset_ += sent_size;

            constexpr std::size_t kSendfileMinChunk = 64 * 1024;
            constexpr std::size_t kSendfileMaxChunk = 1024 * 1024;
            if (sent_size == to_send) {
                sendfile_chunk_size_ = (std::min)(kSendfileMaxChunk, sendfile_chunk_size_ * 2);
            } else if (sent_size < sendfile_chunk_size_ / 2 && sendfile_chunk_size_ > kSendfileMinChunk) {
                sendfile_chunk_size_ = (std::max)(kSendfileMinChunk, sendfile_chunk_size_ / 2);
            }

            (void)check_completed();
            return true;
        }

        if (sent == 0) {
            (void)check_completed();
            return true;
        }

        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
            constexpr std::size_t kSendfileMinChunk = 64 * 1024;
            if (sendfile_chunk_size_ > kSendfileMinChunk) {
                sendfile_chunk_size_ = (std::max)(kSendfileMinChunk, sendfile_chunk_size_ / 2);
            }
            return true;
        }

        return false;
#else
        (void)conn;
        return false;
#endif
    }

    namespace
    {
        constexpr std::size_t kBufferReadChunkSize = 256 * 1024;
    }

    bool HttpUploadFileTask::on_data(yuan::buffer::ByteBuffer *buf)
    {
        if (!attachment_info_ || !buf) {
            return false;
        }

        if (attachment_info_->offset_ >= attachment_info_->length_) {
            return check_completed();
        }

        if (!file_stream_.is_open() || !file_stream_.good()) {
            return false;
        }

        try {
            std::size_t bytes_to_write = std::min<std::size_t>(buf->writable_bytes(), attachment_info_->length_ - attachment_info_->offset_);
            if (bytes_to_write == 0) {
                bytes_to_write = std::min<std::size_t>(kBufferReadChunkSize, attachment_info_->length_ - attachment_info_->offset_);
                buf->ensure_writable(bytes_to_write);
            }

            file_stream_.read(buf->write_ptr(), bytes_to_write);
            const std::size_t read_bytes = static_cast<std::size_t>(file_stream_.gcount());

            if (read_bytes > 0) {
                attachment_info_->offset_ += read_bytes;
                buf->commit(read_bytes);
            }

#ifdef _DEBUG
            LOG_DEBUG("Uploaded {}/{} bytes {:.0f}%", attachment_info_->offset_, attachment_info_->length_,
                      (attachment_info_->offset_ * 100.0) / attachment_info_->length_);
#endif

            return check_completed();

        } catch (const std::exception &e) {
            LOG_ERROR("File read exception: {}", e.what());
            if (file_stream_.is_open()) {
                file_stream_.close();
            }
#ifdef __linux__
            if (file_fd_ >= 0) {
                ::close(file_fd_);
                file_fd_ = -1;
            }
#endif
            return false;
        }
    }

    bool HttpUploadFileTask::is_done() const
    {
        if (!attachment_info_) {
            return false;
        }

        return attachment_info_->offset_ >= attachment_info_->length_;
    }

    bool HttpUploadFileTask::is_good() const
    {
        if (!attachment_info_) {
            return false;
        }

        if (!file_stream_.is_open() || !file_stream_.good()) {
            return false;
        }

        return true;
    }

    bool HttpUploadFileTask::check_completed()
    {
        const bool reached_eof = file_stream_.eof();
        if (attachment_info_->offset_ >= attachment_info_->length_ || reached_eof) {
            file_stream_.close();
#ifdef __linux__
            if (file_fd_ >= 0) {
                ::close(file_fd_);
                file_fd_ = -1;
            }
#endif
            if (completed_callback_) {
                try {
                    completed_callback_();
                    return true;
                } catch (const std::exception &e) {
                    LOG_ERROR("Callback exception: {}", e.what());
                }
            }
        }

        return false;
    }
}
