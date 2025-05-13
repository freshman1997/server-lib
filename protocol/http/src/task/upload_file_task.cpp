#include "task/upload_file_task.h"
#include <iostream>

namespace yuan::net::http 
{
    HttpUploadFileTask::HttpUploadFileTask(std::function<void()> completedCb) : completed_callback_(completedCb)
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

        if (attachment_info_->length_ <= 0) {
            return false;
        }

        file_stream_.open(attachment_info_->origin_file_name_, std::ios::in | std::ios::binary);
        if (!file_stream_.is_open()) {
            return false;
        }

        file_stream_.seekp(0, std::ios::beg);
        return file_stream_.good();
    }

    bool HttpUploadFileTask::on_data(buffer::Buffer *buf)
    {
        if (!attachment_info_ || !buf) {
            return false;
        }

        if (attachment_info_->offset_ >= attachment_info_->length_) {
            return false;
        }

        if (file_stream_.is_open() && file_stream_.good()) {
            // Write the data to the file
            // and update the offset
            std::size_t bytes_to_write = buf->writable_size();
            file_stream_.read(buf->peek_for(), bytes_to_write);

            if (!file_stream_.good()) {
                return false;
            }

            attachment_info_->offset_ += bytes_to_write;

            std::cout << "Uploaded " << attachment_info_->offset_ << " bytes of " << attachment_info_->length_ << " bytes. " << ((attachment_info_->offset_ * 1.0) / (attachment_info_->length_ * 1.0) * 100) << "%" << std::endl;

            if (attachment_info_->offset_ >= attachment_info_->length_) {
                file_stream_.close();
                if (completed_callback_) {
                    completed_callback_();
                }
            }

            return true;
        }

        return false;
    }

    bool HttpUploadFileTask::is_done() const
    {
        return attachment_info_->offset_ >= attachment_info_->length_;
    }
}