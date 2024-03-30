#include "buffer/buffer.h"
#include "buffer/pool.h"

BufferedPool::BufferedPool()
{
    free_list_.push_front(new Buffer);
    free_list_.push_front(new Buffer);
    free_list_.push_front(new Buffer);
}

BufferedPool::~BufferedPool()
{
    for (auto &buffer : using_list_) {
        delete buffer;
    }

    for (auto &buffer : free_list_) {
        delete buffer;
    }
}

Buffer * BufferedPool::allocate(std::size_t sz)
{
    if (free_list_.empty()) {
        free_list_.push_front(new Buffer(sz));
        free_list_.push_front(new Buffer(sz));
        free_list_.push_front(new Buffer(sz));
    }

    Buffer *buf = free_list_.back();
    free_list_.pop_back();
    using_list_.insert(buf);

    return buf;
}

void BufferedPool::free(Buffer *buf)
{
    using_list_.erase(buf);
    if (free_list_.size() > 3) {
        delete buf;
        return;
    }

    buf->resize(8192);
    free_list_.push_front(buf);
}

void BufferedPool::check_size()
{
    std::size_t sz = get_size();

}

std::size_t BufferedPool::get_buffer_size()
{
    std::size_t sz = 0;
    for (const auto &item : using_list_) {
        sz += item->get_buff_size();
    }
    return sz;
}
