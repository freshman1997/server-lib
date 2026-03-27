#include "common/def.h"
#include "nlohmann/json.hpp"

#include <algorithm>
#include <ios>

namespace yuan::net::ftp
{
    std::string_view delimiter = "\r\n";
    std::size_t default_write_buff_size = 1024 * 1024 * 2;
    int default_session_idle_timeout = 10 * 1000;

    FtpFileInfo::~FtpFileInfo()
    {
        if (fstream_) { fstream_->close(); delete fstream_; fstream_ = nullptr; }
    }

    int FtpFileInfo::read_file(std::size_t size, buffer::Buffer *buff)
    {
        if (in_memory_) {
            if (current_progress_ >= memory_content_.size()) { state_ = FileState::processed; return 0; }
            const std::size_t left = memory_content_.size() - current_progress_;
            const std::size_t chunk = std::min(left, size);
            if (buff->writable_size() < chunk) { buff->resize_copy(buff->readable_bytes() + chunk); }
            buff->write_string(memory_content_.data() + current_progress_, chunk);
            current_progress_ += chunk;
            if (current_progress_ >= memory_content_.size()) { state_ = FileState::processed; }
            return static_cast<int>(chunk);
        }

        if (!fstream_) {
            fstream_ = new std::fstream();
            fstream_->open(origin_name_.c_str(), std::ios_base::in | std::ios_base::binary);
            if (!fstream_->good()) { delete fstream_; fstream_ = nullptr; return -1; }
            state_ = FileState::processing;
        }
        fstream_->seekg(static_cast<std::streamoff>(current_progress_), std::ios::beg);
        if (buff->writable_size() < size) { buff->resize_copy(buff->readable_bytes() + size); }
        fstream_->read(buff->buffer_begin(), static_cast<std::streamsize>(size));
        std::size_t read = static_cast<std::size_t>(fstream_->gcount());
        buff->fill(read);
        current_progress_ += read;
        if (current_progress_ >= file_size_) { state_ = FileState::processed; fstream_->close(); delete fstream_; fstream_ = nullptr; }
        return static_cast<int>(read);
    }

    int FtpFileInfo::write_file(buffer::Buffer *buff)
    {
        if (in_memory_) {
            const std::size_t sz = buff->readable_bytes();
            if (sz > 0) { memory_content_.append(buff->peek(), sz); current_progress_ += sz; }
            buff->reset();
            if (file_size_ > 0 && current_progress_ >= file_size_) { state_ = FileState::processed; }
            return static_cast<int>(sz);
        }

        if (!fstream_) {
            fstream_ = new std::fstream();
            auto mode = std::ios_base::out | std::ios_base::binary;
            mode |= append_mode_ ? std::ios_base::app : std::ios_base::trunc;
            fstream_->open(dest_name_.c_str(), mode);
            if (!fstream_->good()) { delete fstream_; fstream_ = nullptr; return -1; }
            state_ = FileState::processing;
        }
        std::size_t sz = buff->readable_bytes();
        fstream_->write(buff->peek(), static_cast<std::streamsize>(sz));
        if (!fstream_->good()) { return -1; }
        current_progress_ += sz;
        buff->reset();
        if (file_size_ > 0 && current_progress_ >= file_size_) { fstream_->flush(); fstream_->close(); delete fstream_; fstream_ = nullptr; state_ = FileState::processed; }
        return static_cast<int>(sz);
    }

    std::string FtpFileInfo::build_cmd_args()
    {
        nlohmann::json jval; jval["type"] = in_memory_ ? "memory" : "file"; jval["size"] = file_size_; jval["progress"] = current_progress_; if (!origin_name_.empty()) { jval["origin"] = origin_name_; } return jval.dump();
    }
}
