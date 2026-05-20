#ifndef NET_FTP_COMMON_DEF_H
#define NET_FTP_COMMON_DEF_H
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include "buffer/byte_buffer.h"
#include "response_code.h"

namespace yuan::net::ftp
{
    extern std::string_view delimiter;
    extern std::size_t default_write_buff_size;
    extern int default_session_idle_timeout;

    enum class WorkMode : char {
        client = 0,
        server
    };
    enum class StreamMode : char {
        Sender,
        Receiver
    };

    struct FtpCommand
    {
        std::string cmd_;
        std::string args_;
    };

    struct FtpCommandResponse
    {
        FtpCommandResponse(FtpResponseCode code, std::string body, bool close = false, bool multiline = false)
            : close_(close), code_(code), body_(std::move(body)), multiline_(multiline)
        {
        }
        bool close_;
        FtpResponseCode code_;
        std::string body_;
        bool multiline_;
    };

    enum class FileState : char {
        init = 0,
        processing,
        processed
    };
    enum class FileType {
        normal_file = 0,
        directory = 1
    };

    struct FtpFileInfo
    {
        StreamMode mode_ = StreamMode::Receiver;
        FileType type_ = FileType::normal_file;
        FileState state_ = FileState::init;
        bool ready_ = false;
        bool in_memory_ = false;
        bool append_mode_ = false;
        std::string origin_name_;
        std::string dest_name_;
        std::string memory_content_;
        std::size_t file_size_ = 0;
        std::size_t current_progress_ = 0;
        std::unique_ptr<std::fstream> fstream_;

        FtpFileInfo() = default;
        FtpFileInfo(const FtpFileInfo &other);
        FtpFileInfo &operator=(const FtpFileInfo &other);
        FtpFileInfo(FtpFileInfo &&) noexcept = default;
        FtpFileInfo &operator=(FtpFileInfo &&) noexcept = default;
        ~FtpFileInfo();
        int read_file(std::size_t size, ::yuan::buffer::ByteBuffer &buff);
        int write_file(::yuan::buffer::ByteBuffer &buff);
        bool is_completed()
        {
            return state_ == FileState::processed;
        }
        std::string build_cmd_args();
    };

    enum class FileStreamState : char {
        init = 0,
        connecting,
        connect_timeout,
        connected,
        connection_error,
        disconnected,
        processing,
        processed,
        file_error,
        idle,
        idle_timeout
    };

    struct User
    {
        bool logined_ = false;
        bool anonymous_ = false;
        std::string username_;
        std::string password_;
    };
}

#endif
