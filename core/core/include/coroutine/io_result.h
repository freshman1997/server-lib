#ifndef __YUAN_COROUTINE_IO_RESULT_H__
#define __YUAN_COROUTINE_IO_RESULT_H__

#include "buffer/byte_buffer.h"

namespace yuan::coroutine
{

    enum class IoStatus {
        success,
        timed_out,
        connection_error,
        connection_closed,
        invalid_state,
    };

    struct ReadResult
    {
        ReadResult()
            : data(0)
        {
        }
        ReadResult(::yuan::buffer::ByteBuffer d, IoStatus s = IoStatus::success)
            : data(std::move(d)), status(s)
        {
        }
        static ReadResult with_status(IoStatus s)
        {
            ReadResult r;
            r.status = s;
            return r;
        }
        ::yuan::buffer::ByteBuffer data;
        IoStatus status = IoStatus::success;
    };

    struct WriteResult
    {
        IoStatus status = IoStatus::success;
    };

    struct DatagramSendResult
    {
        int bytes_sent = 0;
        IoStatus status = IoStatus::success;
    };

} // namespace yuan::coroutine

#endif
