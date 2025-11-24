#include "task/download_file_task.h"
#include <cstdio>
#include <iostream>
#include <filesystem>
#include <cerrno>
#include <cstring>

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
        
        // 验证文件路径安全性
        const auto tmp_path = std::filesystem::path(std::filesystem::u8path(attachment_info_->tmp_file_name_));
        if (tmp_path.is_relative() || tmp_path.string().find("..") != std::string::npos) {
            return false;
        }

        file_stream_.open(tmp_path, std::ios::out | std::ios::binary);
        if (!file_stream_.is_open()) {
            return false;
        }

        file_stream_.seekp(0, std::ios::beg);
        return file_stream_.good();
    }

    int HttpDownloadFileTask::on_data(buffer::BufferReader &reader)
    {
        if (!attachment_info_ || reader.readable_bytes() == 0) {
            return -1;
        }

        if (attachment_info_->offset_ >= attachment_info_->length_) {
            return -1;
        }

        if (file_stream_.is_open() && file_stream_.good()) {
            const std::size_t bytes_to_write = reader.readable_bytes();
            if (bytes_to_write == 0) {
                return check_completed();
            }

            if (const auto write = reader.write_fail_rollback(file_stream_); write < 0 || write != bytes_to_write) {
                return -1;
            }

            file_stream_.flush();
            attachment_info_->offset_ += bytes_to_write;

        #ifndef _DEBUG
            std::cout << "Downloaded " << attachment_info_->offset_ << " bytes of " << attachment_info_->length_ << " bytes. " << ((attachment_info_->offset_ * 1.0) / (attachment_info_->length_ * 1.0) * 100) << "%" << std::endl;
        #endif

            return check_completed();
        }

        return 1;
    }

    bool HttpDownloadFileTask::is_done() const
    {
        return attachment_info_ && attachment_info_->offset_ >= attachment_info_->length_;
    }

    void HttpDownloadFileTask::on_connection_close()
    {
        if (attachment_info_->offset_ < attachment_info_->length_) {
            std::cerr << "Download interrupted, closing file stream." << std::endl;
            if (file_stream_.is_open()) {
                file_stream_.close();
            }

            std::remove(attachment_info_->tmp_file_name_.c_str());
        }
    }

    int HttpDownloadFileTask::check_completed()
    {
        if (attachment_info_->offset_ >= attachment_info_->length_) {
            try {
                file_stream_.close();

                if (std::rename(attachment_info_->tmp_file_name_.c_str(), attachment_info_->origin_file_name_.c_str()) != 0) {
                    std::cerr << "Failed to rename file: " << strerror(errno) << std::endl;
                    return -1;
                }

                if (completed_callback_) {
                    try {
                        completed_callback_();
                    } catch (const std::exception& e) {
                        std::cerr << "Callback exception: " << e.what() << std::endl;
                        return -1;
                    }
                }

                return 0;
            } catch (const std::exception& e) {
                std::cerr << "File rename exception: " << e.what() << std::endl;
            }
        }
        
        return -1;
    }
}