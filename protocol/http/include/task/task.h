#ifndef __NET_HTTP_TASK_H__
#define __NET_HTTP_TASK_H__

#include "buffer/buffer.h"
namespace yuan::net::http 
{
    enum class HttpTaskType
    {
        none_,
        download_file_,
        upload_file_,
    };
    
    class HttpTask
    {
    public:
        virtual ~HttpTask() {}
        virtual bool on_data(buffer::Buffer *buf) = 0;
        virtual bool init() { return true; }
        virtual void reset() {}
        virtual bool is_done() const { return false; }

        virtual HttpTaskType get_task_type() const
        {
            return HttpTaskType::none_;
        }
    };
}

#endif // __NET_HTTP_TASK_H__