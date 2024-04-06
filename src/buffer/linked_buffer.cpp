#include "buffer/linked_buffer.h"
#include "buffer/pool.h"
#include "singleton/singleton.h"
#include "buffer/buffer.h"

LinkedBuffer::LinkedBuffer()
{
    allocate_buffer();
}

LinkedBuffer::~LinkedBuffer()
{
    for (const auto &item : buffers_) {
        singleton::Singleton<BufferedPool>().free(item);
    }
}

Buffer * LinkedBuffer::get_current_buffer()
{
    return buffers_.back();
}

Buffer * LinkedBuffer::allocate_buffer(std::size_t sz)
{
    Buffer *buf = singleton::Singleton<BufferedPool>().allocate(sz);
    buffers_.push_front(buf);
    return buf;
}

void LinkedBuffer::free_current_buffer()
{
    if (buffers_.size() > 1) {
        Buffer *buf = buffers_.back();
        buffers_.pop_back();
        singleton::Singleton<BufferedPool>().free(buf);
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

