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
    buffers_.clear();
}

LinkedBuffer & LinkedBuffer::operator=(LinkedBuffer &linkBuff)
{
    free_all_buffers();
    buffers_ = linkBuff.buffers_;
    return *this;
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

void LinkedBuffer::clear()
{
    buffers_.clear();
}

void LinkedBuffer::free_all_buffers()
{
    for (const auto &item : buffers_) {
        BufferedPool::get_instance()->free(item);
    }
    buffers_.clear();
}

void LinkedBuffer::foreach(std::function<bool (Buffer *buff)> func)
{
    for (auto it = buffers_.rbegin(); it != buffers_.rend(); ++it) {
        if (!func(*it)) {
            break;
        }
    }
}

void LinkedBuffer::keep_one_buffer()
{
    if (buffers_.empty()) {
        allocate_buffer();
        return;
    }

    Buffer *buff = buffers_.back();
    buff->reset();
    if (buffers_.size() == 1) {
        return;
    }
    buffers_.pop_back();
    free_all_buffers();
    append_buffer(buff);
}