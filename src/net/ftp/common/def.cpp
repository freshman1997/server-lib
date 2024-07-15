#include "net/ftp/common/def.h"
#include "nlohmann/json.hpp"
#include <cstddef>
#include <iostream>
#include <string>

namespace net::ftp 
{

    FileInfo::~FileInfo()
    {
        if (fstream_) {
            fstream_->close();
            delete fstream_;
            fstream_ = nullptr;
        }
    }

    int FileInfo::read_file(std::size_t size, Buffer *buff)
    {
        if (!fstream_) {
            fstream_ = new std::fstream();
            fstream_->open(origin_name_.c_str(), std::ios_base::in | std::ios_base::binary);
            if (fstream_->bad()) {
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
        std::cout << "done >> " << current_progress_ << ", " << file_size_ << '\n';
        return read;
    }

    int FileInfo::write_file(Buffer *buff)
    {
        if (!fstream_) {
            fstream_ = new std::fstream();
            fstream_->open(dest_name_.c_str(), std::ios_base::out);
            if (fstream_->bad()) {
                delete fstream_;
                fstream_ = nullptr;
                return -1;
            }
            state_ = FileState::processing;
        }

        fstream_->write(buff->peek(), buff->readable_bytes());
        std::size_t read = fstream_->gcount();
        if (current_progress_ >= file_size_) {
            state_ = FileState::processed;
            fstream_->close();
            delete fstream_;
            fstream_ = nullptr;
        }

        return read;
    }

    std::string FileInfo::build_cmd_args()
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