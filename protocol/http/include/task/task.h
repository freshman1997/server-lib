#ifndef __NET_HTTP_TASK_H__
#define __NET_HTTP_TASK_H__

#include "buffer/byte_buffer.h"

namespace yuan::net
{
    class Connection;
}

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
        virtual bool on_data(::yuan::buffer::ByteBuffer *buf) { return false; }
        virtual bool on_data(const ::yuan::buffer::ByteBuffer &buf) { return false; }
        virtual bool write_to_connection(::yuan::net::Connection *conn)
        {
            (void)conn;
            return false;
        }
        virtual bool init() { return true; }
        virtual void reset() {}
        virtual bool is_done() const { return false; }
        virtual void on_connection_close() {}
        virtual bool is_good() const { return true; }

        virtual HttpTaskType get_task_type() const
        {
            return HttpTaskType::none_;
        }
    };
}

#endif // __NET_HTTP_TASK_H__
