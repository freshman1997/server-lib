#include "task/upload_file_task.h"
#include <iostream>
#include <filesystem>
#include <algorithm>
#include <cstring>

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
        
        // 验证文件路径安全性，防止路径遍历攻击
        const auto file_path = std::filesystem::path(std::filesystem::u8path(attachment_info_->origin_file_name_));
        if (file_path.is_relative()) {
            // 相对路径可能导致安全问题
            return false;
        }
        
        // 检查文件是否存在
        if (!std::filesystem::exists(file_path)) {
            return false;
        }
        
        // 检查文件大小是否合理
        std::error_code ec;
        if (const auto file_size = std::filesystem::file_size(file_path, ec); ec || file_size > attachment_info_->length_) {
            return false;
        }
        
        file_stream_.open(file_path, std::ios::in | std::ios::binary);
        if (!file_stream_.is_open()) {
            return false;
        }

        file_stream_.seekg(0, std::ios::beg);
        return file_stream_.good();
    }

    bool HttpUploadFileTask::on_data(buffer::Buffer *buf)
    {
        // 参数验证
        if (!attachment_info_ || !buf) {
            return false;
        }

        // 检查是否已完成上传
        if (attachment_info_->offset_ >= attachment_info_->length_) {
            return check_completed();
        }

        // 检查缓冲区大小
        if (buf->writable_size() == 0) {
            return false;
        }

        // 检查文件流状态
        if (!file_stream_.is_open() || !file_stream_.good()) {
            return false;
        }
        
        try {
            // Write the data to the file
            // and update the offset
            std::size_t bytes_to_write = std::min<std::size_t>(buf->writable_size(), attachment_info_->length_ - attachment_info_->offset_);
            if (bytes_to_write == 0) {
                return false;
            }
            
            file_stream_.read(buf->buffer_begin(), bytes_to_write);
            
            std::size_t read_bytes = file_stream_.gcount();
            if (read_bytes > 0) {
                attachment_info_->offset_ += read_bytes;
                buf->fill(read_bytes);
            }

        #ifdef _DEBUG
            std::cout << "Uploaded " << attachment_info_->offset_ << " bytes of " << attachment_info_->length_ << " bytes. " << ((attachment_info_->offset_ * 1.0) / (attachment_info_->length_ * 1.0) * 100) << "%" << std::endl;
        #endif

            return check_completed();

        } catch (const std::exception& e) {
            // 处理文件读取过程中的异常
            std::cerr << "File read exception: " << e.what() << std::endl;
            if (file_stream_.is_open()) {
                file_stream_.close();
            }
            return false;
        }

        return false;
    }

    bool HttpUploadFileTask::is_done() const
    {
        // 检查是否有附件信息
        if (!attachment_info_) {
            return false;
        }
        
        // 检查是否已完成上传或达到文件大小
        return attachment_info_->offset_ >= attachment_info_->length_;
    }

    bool HttpUploadFileTask::is_good() const
    {
        if (!attachment_info_) {
            return false;
        }

        if (!file_stream_.is_open() || !file_stream_.good()) {
            return false;
        }

        return true;
    }

    bool HttpUploadFileTask::check_completed()
    {
        // 检查是否完成上传或到达文件末尾
        if (attachment_info_->offset_ >= attachment_info_->length_ || file_stream_.eof()) {
            file_stream_.close();
            if (completed_callback_) {
                try {
                    completed_callback_();
                    return true;
                } catch (const std::exception& e) {
                    // 记录回调异常但不中断流程
                    std::cerr << "Callback exception: " << e.what() << std::endl;
                }
            }
        }

        return false;
    }
}