#include "task/download_file_task.h"
#include "logger.h"
#include "platform/native_platform.h"
#include <cstdio>
#include <filesystem>

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
        
        // 验证文件路径安全性，确保不会写入到不安全的位置
        auto tmp_path = std::filesystem::path(std::filesystem::u8path(attachment_info_->tmp_file_name_));
        if (tmp_path.string().find("..") != std::string::npos) {
            return false;
        }
        if (tmp_path.is_relative()) {
            tmp_path = std::filesystem::temp_directory_path() / tmp_path.filename();
            attachment_info_->tmp_file_name_ = tmp_path.string();
        }

        file_stream_.open(tmp_path, std::ios::out | std::ios::binary);
        if (!file_stream_.is_open()) {
            return false;
        }

        file_stream_.seekp(0, std::ios::beg);
        return file_stream_.good();
    }

    bool HttpDownloadFileTask::on_data(const yuan::buffer::ByteBuffer &buf)
    {
        if (!attachment_info_) {
            return false;
        }

        if (attachment_info_->offset_ >= attachment_info_->length_) {
            return false;
        }

        if (file_stream_.is_open() && file_stream_.good()) {
            std::size_t bytes_to_write = buf.readable_bytes();
            if (bytes_to_write == 0) {
                return check_completed();
            }

            file_stream_.write(buf.read_ptr(), bytes_to_write);
            file_stream_.flush();
            attachment_info_->offset_ += bytes_to_write;

        #ifndef _DEBUG
            LOG_DEBUG("Downloaded {}/{} bytes {:.0f}%", attachment_info_->offset_, attachment_info_->length_, (attachment_info_->offset_ * 100.0) / attachment_info_->length_);
        #endif

            return check_completed();
        }

        return false;
    }

    bool HttpDownloadFileTask::is_done() const
    {
        return attachment_info_ && attachment_info_->offset_ >= attachment_info_->length_;
    }

    void HttpDownloadFileTask::on_connection_close()
    {
        if (!attachment_info_) {
            return;
        }
        if (attachment_info_->offset_ < attachment_info_->length_) {
            LOG_WARN("Download interrupted, closing file stream.");
            if (file_stream_.is_open()) {
                file_stream_.close();
            }

            std::remove(attachment_info_->tmp_file_name_.c_str());
        }
    }

    bool HttpDownloadFileTask::check_completed()
    {
        if (attachment_info_->offset_ >= attachment_info_->length_) {
            file_stream_.close();
            if (completed_callback_) {
                try {
                    completed_callback_();
                } catch (const std::exception& e) {
                    LOG_ERROR("Callback exception: {}", e.what());
                    return false;
                }
            }

            try {
                if (std::rename(attachment_info_->tmp_file_name_.c_str(), attachment_info_->origin_file_name_.c_str()) != 0) {
                    const int rename_error = yuan::platform::GetLastSystemError();
                    LOG_ERROR("Failed to rename file: {} ({})",
                              rename_error,
                              yuan::platform::DescribeNativeError(rename_error));
                    return false;
                }
                return true;
            } catch (const std::exception& e) {
                LOG_ERROR("File rename exception: {}", e.what());
            }
        }
        
        return false;
    }
}
