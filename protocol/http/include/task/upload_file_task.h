#ifndef __UPLOAD_FILE_TASK_H__
#define __UPLOAD_FILE_TASK_H__
#include "attachment/attachment.h"
#include "task.h"
#include <cstdint>
#include <fstream>
#include <functional>
#include <memory>

namespace yuan::net::http 
{
    class HttpUploadFileTask : public HttpTask
    {
    public:
        HttpUploadFileTask() = delete;
        explicit HttpUploadFileTask(std::function<void()> completedCb);
        ~HttpUploadFileTask() override
        {
            if (file_stream_.is_open()) {
                file_stream_.close();
            }
        }

    public:
        void reset() override
        {
            HttpTask::reset();
            if (file_stream_.is_open()) {
                file_stream_.close();
            }
        }

        bool init() override;

        bool on_data(::yuan::buffer::ByteBuffer *buf) override;

        bool write_to_connection(::yuan::net::Connection *conn) override;

        void set_sendfile_enabled(bool enabled)
        {
            sendfile_enabled_ = enabled;
        }

        void set_write_timeout_ms(uint32_t timeout_ms)
        {
            write_timeout_ms_ = timeout_ms;
        }

        HttpTaskType get_task_type() const override
        {
            return HttpTaskType::upload_file_;
        }

        std::shared_ptr<AttachmentInfo> get_attachment_info()
        {
            return attachment_info_;
        }

        void set_attachment_info(std::shared_ptr<AttachmentInfo> info)
        {
            attachment_info_ = info;
        }

        bool is_done() const override;

        virtual bool is_good() const override;

    private:
        bool check_completed();

    private:
        std::shared_ptr<AttachmentInfo> attachment_info_;
#ifndef _WIN32
        int file_fd_ = -1;
#endif
        std::fstream file_stream_;
        std::function<void()> completed_callback_;
        bool sendfile_enabled_ = false;
#ifdef __linux__
        std::size_t sendfile_chunk_size_ = 256 * 1024;
#endif
        uint32_t write_timeout_ms_ = 0;
        uint64_t stalled_since_ms_ = 0;
    };
}

#endif // __UPLOAD_FILE_TASK_H__
