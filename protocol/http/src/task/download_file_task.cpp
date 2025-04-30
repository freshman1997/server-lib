#include "task/download_file_task.h"
#include <iostream>

namespace yuan::net::http 
{
    HttpDownloadFileTask::HttpDownloadFileTask(std::function<void()> completedCb)
    {
        completed_callback_ = std::move(completedCb);
    }

    bool HttpDownloadFileTask::init()
    {
        if (!attachment_info_) {
            return false;
        }

        if (attachment_info_->tmp_file_name_.empty()) {
            return false;
        }

        if (attachment_info_->length_ <= 0) {
            return false;
        }

        file_stream_.open(attachment_info_->tmp_file_name_, std::ios::out | std::ios::binary);
        if (!file_stream_.is_open()) {
            return false;
        }

        file_stream_.seekp(0, std::ios::beg);
        return file_stream_.good();
    }

    bool HttpDownloadFileTask::on_data(buffer::Buffer *buf)
    {
        if (!attachment_info_ || !buf) {
            return false;
        }

        if (attachment_info_->offset_ >= attachment_info_->length_) {
            return false;
        }

        if (file_stream_.is_open() && file_stream_.good()) {
            std::size_t bytes_to_write = buf->readable_bytes();
            file_stream_.write(buf->peek(), bytes_to_write);
            file_stream_.flush();
            attachment_info_->offset_ += bytes_to_write;

            std::cout << "Downloaded " << attachment_info_->offset_ << " bytes of " << attachment_info_->length_ << " bytes. " << ((attachment_info_->offset_ * 1.0) / (attachment_info_->length_ * 1.0) * 100) << "%" << std::endl;

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

    bool HttpDownloadFileTask::is_done() const
    {
        return attachment_info_ && attachment_info_->offset_ >= attachment_info_->length_;
    }
}