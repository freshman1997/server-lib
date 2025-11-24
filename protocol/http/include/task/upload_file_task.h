#ifndef __UPLOAD_FILE_TASK_H__
#define __UPLOAD_FILE_TASK_H__
#include "attachment/attachment.h"
#include "task.h"
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

        int on_data(buffer::BufferReader &buff) override;

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
        std::ifstream file_stream_;
        std::function<void()> completed_callback_;
    };
}

#endif // __UPLOAD_FILE_TASK_H__
