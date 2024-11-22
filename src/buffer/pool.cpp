#include "buffer/buffer.h"
#include <iostream>
#include "buffer/pool.h"

constexpr size_t MAX_FREE_LIST = 3;

BufferedPool::BufferedPool()
{
    
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

Buffer * BufferedPool::allocate(const std::size_t sz)
{
    if (free_list_.empty()) {
        free_list_.push_front(new Buffer());
        free_list_.push_front(new Buffer());
        free_list_.push_front(new Buffer());
    }

    Buffer *buf = free_list_.back();
    free_list_.pop_back();
    buf->resize(sz);
    using_list_.insert(buf);

    //check_size();

    return buf;
}

void BufferedPool::free(Buffer *buf)
{
    if (!buf) {
        return;
    }
    
    using_list_.erase(buf);
    if (free_list_.size() > MAX_FREE_LIST) {
        delete buf;
        return;
    }

    buf->reset();
    buf->resize(8192);
    free_list_.push_front(buf);
    //check_size();
}

void BufferedPool::check_size()
{
    std::size_t sz = get_size();
    std::cout << "============> " << get_buffer_size() << '\n';
}

std::size_t BufferedPool::get_buffer_size()
{
    std::size_t sz = 0;
    for (const auto &item : using_list_) {
        sz += item->get_buff_size();
    }

    for (const auto &item : free_list_) {
        sz += item->get_buff_size();
    }

    return sz;
}

void BufferedPool::buffer_append_size(Buffer *buf, size_t size)
{
    if (buf) {
        buf->append_size(size);
        //check_size();
    }
}