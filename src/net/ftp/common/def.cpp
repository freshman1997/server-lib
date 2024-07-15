#include "net/ftp/common/def.h"
#include "nlohmann/json.hpp"
#include <cstddef>
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
            fstream_->open(origin_name_.c_str(), std::ios_base::in);
            if (fstream_->bad()) {
                delete fstream_;
                return -1;
            }
            state_ = FileState::processing;
        }

        fstream_->seekg(current_progress_);
        fstream_->read(buff->peek_for(), buff->writable_size());
        std::size_t read = fstream_->gcount();
        buff->fill(read);

        current_progress_ += read;
        if (current_progress_ >= file_size_) {
            state_ = FileState::processed;
            fstream_->close();
            delete fstream_;
        }

        return read;
    }

    int FileInfo::write_file(Buffer *buff)
    {
        if (!fstream_) {
            fstream_ = new std::fstream();
            fstream_->open(dest_name_.c_str(), std::ios_base::out);
            if (fstream_->bad()) {
                delete fstream_;
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
        }

        return read;
    }

    std::string FileInfo::build_cmd_args()
    {
        nlohmann::json jval;
        jval["type"] = "file";
        jval["size"] = std::to_string(file_size_);
        jval["progress"] = std::to_string(current_progress_);

        if (!origin_name_.empty()) {
            jval["origin"] = origin_name_;
        }

        return jval.dump();
    }
}