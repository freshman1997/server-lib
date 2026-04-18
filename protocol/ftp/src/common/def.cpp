#include "common/def.h"
#include "nlohmann/json.hpp"

#include <algorithm>
#include <ios>
#include <memory>

namespace yuan::net::ftp
{
    std::string_view delimiter = "\r\n";
    std::size_t default_write_buff_size = 1024 * 1024 * 2;
    int default_session_idle_timeout = 10 * 1000;

    FtpFileInfo::FtpFileInfo(const FtpFileInfo &other)
        : mode_(other.mode_),
          type_(other.type_),
          state_(other.state_),
          ready_(other.ready_),
          in_memory_(other.in_memory_),
          append_mode_(other.append_mode_),
          origin_name_(other.origin_name_),
          dest_name_(other.dest_name_),
          memory_content_(other.memory_content_),
          file_size_(other.file_size_),
          current_progress_(other.current_progress_)
    {
    }

    FtpFileInfo &FtpFileInfo::operator=(const FtpFileInfo &other)
    {
        if (this != &other) {
            mode_ = other.mode_;
            type_ = other.type_;
            state_ = other.state_;
            ready_ = other.ready_;
            in_memory_ = other.in_memory_;
            append_mode_ = other.append_mode_;
            origin_name_ = other.origin_name_;
            dest_name_ = other.dest_name_;
            memory_content_ = other.memory_content_;
            file_size_ = other.file_size_;
            current_progress_ = other.current_progress_;
            fstream_.reset();
        }
        return *this;
    }

    FtpFileInfo::~FtpFileInfo()
    {
        if (fstream_) {
            fstream_->close();
            fstream_.reset();
        }
    }

    int FtpFileInfo::read_file(std::size_t size, ::yuan::buffer::ByteBuffer & buff)
    {
        if (in_memory_) {
            if (current_progress_ >= memory_content_.size()) {
                state_ = FileState::processed;
                return 0;
            }
            const std::size_t left = memory_content_.size() - current_progress_;
            const std::size_t chunk = std::min(left, size);
            buff.append(memory_content_.data() + current_progress_, chunk);
            current_progress_ += chunk;
            if (current_progress_ >= memory_content_.size()) {
                state_ = FileState::processed;
            }
            return static_cast<int>(chunk);
        }

        if (!fstream_) {
            fstream_ = std::make_unique<std::fstream>();
            fstream_->open(origin_name_.c_str(), std::ios_base::in | std::ios_base::binary);
            if (!fstream_->good()) {
                fstream_.reset();
                return -1;
            }
            state_ = FileState::processing;
        }

        fstream_->seekg(static_cast<std::streamoff>(current_progress_), std::ios::beg);
        buff.ensure_writable(size);
        fstream_->read(buff.write_ptr(), static_cast<std::streamsize>(size));
        std::size_t read = static_cast<std::size_t>(fstream_->gcount());
        buff.commit(read);
        current_progress_ += read;
        if (fstream_->eof() || read == 0 || current_progress_ >= file_size_) {
            state_ = FileState::processed;
            fstream_->close();
            fstream_.reset();
        }
        return static_cast<int>(read);
    }

    int FtpFileInfo::write_file(::yuan::buffer::ByteBuffer & buff)
    {
        if (in_memory_) {
            const std::size_t sz = buff.readable_bytes();
            if (sz > 0) {
                memory_content_.append(buff.read_ptr(), sz);
                current_progress_ += sz;
            }
            buff.clear();
            if (file_size_ > 0 && current_progress_ >= file_size_) {
                state_ = FileState::processed;
            }
            return static_cast<int>(sz);
        }

        if (!fstream_) {
            fstream_ = std::make_unique<std::fstream>();
            auto mode = std::ios_base::out | std::ios_base::binary;
            mode |= append_mode_ ? std::ios_base::app : std::ios_base::trunc;
            fstream_->open(dest_name_.c_str(), mode);
            if (!fstream_->good()) {
                fstream_.reset();
                return -1;
            }
            state_ = FileState::processing;
        }

        std::size_t sz = buff.readable_bytes();
        fstream_->write(buff.read_ptr(), static_cast<std::streamsize>(sz));
        if (!fstream_->good()) {
            fstream_.reset();
            return -1;
        }
        current_progress_ += sz;
        buff.clear();
        if (file_size_ > 0 && current_progress_ >= file_size_) {
            fstream_->flush();
            fstream_->close();
            fstream_.reset();
            state_ = FileState::processed;
        }
        return static_cast<int>(sz);
    }

    std::string FtpFileInfo::build_cmd_args()
    {
        nlohmann::json jval;
        jval["type"] = in_memory_ ? "memory" : "file";
        jval["size"] = file_size_;
        jval["progress"] = current_progress_;
        if (!origin_name_.empty()) {
            jval["origin"] = origin_name_;
        }
        return jval.dump();
    }
}
