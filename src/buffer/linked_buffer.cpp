#include "buffer/linked_buffer.h"
#include "buffer/pool.h"
#include "singleton/singleton.h"


LinkedBuffer::LinkedBuffer()
{
    allocate_buffer();
}

LinkedBuffer::~LinkedBuffer()
{
    for (const auto &item : buffers_) {
        singleton::singleton<BufferedPool>().free(item);
    }
}

Buffer * LinkedBuffer::get_current_buffer()
{
    return buffers_.back();
}

Buffer * LinkedBuffer::allocate_buffer(std::size_t sz)
{
    Buffer *buf = singleton::singleton<BufferedPool>().allocate(sz);
    buffers_.push_front(buf);
    return buf;
}

void LinkedBuffer::free_current_buffer()
{
    if (buffers_.size() > 1) {
        Buffer *buf = buffers_.back();
        buffers_.pop_back();
        singleton::singleton<BufferedPool>().free(buf);
    }
}

void LinkedBuffer::append_buffer(Buffer *buf)
{
    buffers_.push_front(buf);
}

