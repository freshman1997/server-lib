#ifndef __BUFFER_POOL_H__
#define __BUFFER_POOL_H__
#include <list>
#include <set>
#include <memory>

#include "../singleton/singleton.h"

namespace yuan::buffer
{
    class Buffer;

    class BufferedPool : public singleton::Singleton<BufferedPool>, public std::enable_shared_from_this<BufferedPool>
    {
    public:
        std::size_t get_size() const
        {
            return using_list_.size() + free_list_.size();
        }

        std::size_t get_buffer_size() const;

        buffer::Buffer * allocate(std::size_t sz = 8192);

        void free(buffer::Buffer *buf);

        void buffer_append_size(buffer::Buffer *buf, size_t size = 0);

        BufferedPool();
        ~BufferedPool();

        BufferedPool(BufferedPool &) = delete;
        BufferedPool & operator=(BufferedPool &) = delete;

    private:
        void check_size() const;

    private:
        std::set<buffer::Buffer *> using_list_;
        std::list<buffer::Buffer *> free_list_;
        std::mutex buffer_mutex_;
    };
}
#endif