#include "common/def.h"
#include "nlohmann/json.hpp"
#include <cstddef>
#include <ios>
#include <iostream>
#include <string>

namespace net::ftp 
{

    std::string_view delimiter = "\r\n";
    std::size_t default_write_buff_size = 1024 * 1024 * 2;
    int default_session_idle_timeout = 10 * 1000;

    FtpFileInfo::~FtpFileInfo()
    {
        if (fstream_) {
            fstream_->close();
            delete fstream_;
            fstream_ = nullptr;
        }
    }

    int FtpFileInfo::read_file(std::size_t size, Buffer *buff)
    {
        if (!fstream_) {
            fstream_ = new std::fstream();
            fstream_->open(origin_name_.c_str(), std::ios_base::in | std::ios_base::binary);
            if (!fstream_->good()) {
                delete fstream_;
                fstream_ = nullptr;
                return -1;
            }
            state_ = FileState::processing;
        }

        fstream_->seekg(current_progress_, std::ios::beg);
        if (buff->writable_size() < size) {
            buff->resize(size);
        }
        fstream_->read(buff->peek_for(), buff->writable_size());
        std::size_t read = fstream_->gcount();
        buff->fill(read);

        current_progress_ += read;
        if (current_progress_ >= file_size_) {
            state_ = FileState::processed;
            fstream_->close();
            delete fstream_;
            fstream_ = nullptr;
        }
        return read;
    }

    int FtpFileInfo::write_file(Buffer *buff)
    {
        if (!fstream_) {
            fstream_ = new std::fstream();
            fstream_->open(dest_name_.c_str(), std::ios_base::out | std::ios_base::binary);
            if (!fstream_->good()) {
                delete fstream_;
                fstream_ = nullptr;
                return -1;
            }
            state_ = FileState::processing;
        }

        std::size_t sz = buff->readable_bytes();
        fstream_->write(buff->peek(), sz);
        std::size_t cnt = fstream_->gcount();
        std::size_t written = sz;
        current_progress_ += written;
        if (current_progress_ >= file_size_) {
            fstream_->flush();
            fstream_->close();
            delete fstream_;
            fstream_ = nullptr;
            state_ = FileState::processed;
        }
        return written;
    }

    std::string FtpFileInfo::build_cmd_args()
    {
        nlohmann::json jval;
        jval["type"] = "file";
        jval["size"] = file_size_;
        jval["progress"] = current_progress_;

        if (!origin_name_.empty()) {
            jval["origin"] = origin_name_;
        }

        return jval.dump();
    }
}