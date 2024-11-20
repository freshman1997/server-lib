#ifndef __BUFFER_POOL_H__
#define __BUFFER_POOL_H__
#include <list>
#include <set>
#include <memory>

#include "../singleton/singleton.h"

class Buffer;

class BufferedPool : public singleton::Singleton<BufferedPool>, public std::enable_shared_from_this<BufferedPool>
{
public:
    std::size_t get_size() const
    {
        return using_list_.size() + free_list_.size();
    }

    std::size_t get_buffer_size();

    Buffer * allocate(std::size_t sz = 8192);

    void free(Buffer *buf);

    void buffer_append_size(Buffer *buf, size_t size = 0);

    BufferedPool();
    ~BufferedPool();

    BufferedPool(BufferedPool &) = delete;
    BufferedPool & operator=(BufferedPool &) = delete;

private:
    void check_size();

private:
    std::set<Buffer *> using_list_;
    std::list<Buffer *> free_list_;
};

#endif