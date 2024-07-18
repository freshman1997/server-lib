#include "buffer/linked_buffer.h"
#include "buffer/pool.h"
#include "buffer/buffer.h"

LinkedBuffer::LinkedBuffer()
{
    allocate_buffer();
}

LinkedBuffer::~LinkedBuffer()
{
    for (const auto &item : buffers_) {
        BufferedPool::get_instance()->free(item);
    }
}

Buffer * LinkedBuffer::get_current_buffer()
{
    return buffers_.back();
}

Buffer * LinkedBuffer::allocate_buffer(std::size_t sz)
{
    Buffer *buf = BufferedPool::get_instance()->allocate(sz);
    buffers_.push_front(buf);
    return buf;
}

void LinkedBuffer::free_current_buffer(Buffer *replaceBuff)
{
    if (replaceBuff || buffers_.size() > 1) {
        Buffer *buf = buffers_.back();
        buffers_.pop_back();
        BufferedPool::get_instance()->free(buf);
    }

    if (replaceBuff) {
        append_buffer(replaceBuff);
    }
}

void LinkedBuffer::append_buffer(Buffer *buf)
{
    buffers_.push_front(buf);
}

Buffer * LinkedBuffer::take_current_buffer()
{
    Buffer *buf = buffers_.back();
    buf->reset_read_index(0);
    buffers_.pop_back();

    allocate_buffer();
    
    return buf;
}

