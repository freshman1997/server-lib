#ifndef __NET_FTP_COMMON_DEF_H__
#define __NET_FTP_COMMON_DEF_H__
#include <fstream>
#include <string>
#include <string_view>

#include "buffer/buffer.h"
#include "response_code.h"

namespace yuan::net::ftp 
{
    extern std::string_view delimiter;
    extern std::size_t default_write_buff_size;
    extern int default_session_idle_timeout;

    enum class WorkMode : char { client = 0, server };
    enum class StreamMode : char { Sender, Receiver };

    struct FtpCommand { std::string cmd_; std::string args_; };

    struct FtpCommandResponse
    {
        FtpCommandResponse(FtpResponseCode code, const std::string &body, bool close = false) : code_(code), body_(std::move(body)), close_(close) {}
        bool close_;
        FtpResponseCode code_;
        std::string body_;
    };

    enum class FileState : char { init = 0, processing, processed };
    enum class FileType { normal_file = 0, directionary = 1 };

    struct FtpFileInfo
    {
        StreamMode mode_ = StreamMode::Receiver;
        FileType type_ = FileType::normal_file;
        FileState state_ = FileState::init;
        bool ready_ = false;
        bool in_memory_ = false;
        bool append_mode_ = false;
        std::string origin_name_ = "";
        std::string dest_name_ = "";
        std::string memory_content_ = "";
        std::size_t file_size_ = 0;
        std::size_t current_progress_ = 0;
        std::fstream *fstream_ = nullptr;

        ~FtpFileInfo();
        int read_file(std::size_t size, buffer::Buffer *buff);
        int write_file(buffer::Buffer *buff);
        bool is_completed() { return state_ == FileState::processed; }
        std::string build_cmd_args();
    };

    enum class FileSteamState : char { init = 0, connecting, connect_timeout, connected, connection_error, disconnected, processing, processed, file_error, idle, idle_timeout };

    struct User { bool logined_ = false; bool anoyned_ = false; std::string username_; std::string password_; };
    enum class StreamStructure : char { normal = 0, record, page };
    enum class TransferMode : char { stream = 0, chuck, compress };
    enum class FileSystemType { ms_dos = 0, unix_like };

    struct FileInfo
    {
        int grant_;
        std::size_t size_;
        time_t date_;
        std::string group_;
        std::string user_;
        std::string file_name_;
    };
}

#endif
