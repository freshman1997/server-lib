#include "buffer/linked_buffer.h"
#include "buffer/pool.h"
#include "buffer/buffer.h"
namespace yuan::buffer
{
    LinkedBuffer::LinkedBuffer()
    {
        allocate_buffer();
    }

    LinkedBuffer::~LinkedBuffer()
    {
        free_all_buffers();
    }

    LinkedBuffer & LinkedBuffer::operator=(LinkedBuffer &linkBuff)
    {
        free_all_buffers();
        buffers_ = linkBuff.buffers_;
        return *this;
    }

    buffer::Buffer * LinkedBuffer::get_current_buffer()
    {
        if (buffers_.empty()) {
            allocate_buffer();
        }
        return buffers_.back();
    }

    buffer::Buffer * LinkedBuffer::allocate_buffer(std::size_t sz)
    {
        buffer::Buffer *buf = BufferedPool::get_instance()->allocate(sz);
        buffers_.push_front(buf);
        return buf;
    }

    void LinkedBuffer::free_current_buffer()
    {
        buffer::Buffer *buf = buffers_.back();
        if (buffers_.size() > 1) {
            buffers_.pop_back();
            BufferedPool::get_instance()->free(buf);
        } else {
            buf->reset();
            buf->resize(8192);
        }
    }

    void LinkedBuffer::append_buffer(buffer::Buffer *buf)
    {
        buffers_.push_front(buf);
    }

    buffer::Buffer * LinkedBuffer::take_current_buffer()
    {
        buffer::Buffer *buf = buffers_.back();
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
        for (buffer::Buffer *item : buffers_) {
            BufferedPool::get_instance()->free(item);
        }
        buffers_.clear();
    }

    void LinkedBuffer::foreach(std::function<bool (buffer::Buffer *buff)> func)
    {
        for (auto it = buffers_.rbegin(); it != buffers_.rend(); ++it) {
            if ((*it)->empty()) {
                continue;
            }

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

        buffer::Buffer *buff = buffers_.back();
        buff->reset();
        if (buffers_.size() == 1) {
            return;
        }
        buffers_.pop_back();
        free_all_buffers();
        append_buffer(buff);
    }

    std::vector<buffer::Buffer *> LinkedBuffer::to_vector(bool clear)
    {
        std::vector<buffer::Buffer *> ret;
        for (auto it = buffers_.rbegin(); it != buffers_.rend(); ++it) {
            ret.push_back(*it);
        }

        if (clear) {
            buffers_.clear();
        }
        
        return ret;
    }
}