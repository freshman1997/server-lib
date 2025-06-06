#ifndef __DOWNLOAD_FILE_TASK_H__
#define __DOWNLOAD_FILE_TASK_H__
#include "attachment/attachment.h"
#include "task/task.h"
#include <fstream>
#include <functional>
#include <memory>

namespace yuan::net::http 
{
    class HttpDownloadFileTask : public HttpTask
    {
    public:
        HttpDownloadFileTask(std::function<void()> completedCb);
        ~HttpDownloadFileTask() override
        {
            if (file_stream_.is_open()) {
                file_stream_.close();
            }
        }

    public:
        virtual bool init() override;

        virtual bool on_data(buffer::Buffer *buf) override;

        virtual HttpTaskType get_task_type() const override
        {
            return HttpTaskType::download_file_;
        }

        virtual bool is_done() const override;

        virtual void on_connection_close() override;

    public:
        std::shared_ptr<AttachmentInfo> get_attachemnt_info()
        {
            return attachment_info_;
        }

        void set_attachment_info(std::shared_ptr<AttachmentInfo> info)
        {
            attachment_info_ = info;
        }

    private:
        std::shared_ptr<AttachmentInfo> attachment_info_;
        std::fstream file_stream_;
        std::function<void()> completed_callback_;
    };
}

#endif // __DOWNLOAD_FILE_TASK_H__