#ifndef __LINKED_BUFF_H__
#define __LINKED_BUFF_H__
#include <functional>
#include <list>
#include <vector>

namespace yuan::buffer
{
    class Buffer;
    class LinkedBuffer
    {
    public:
        LinkedBuffer();
        ~LinkedBuffer();

    public:
        LinkedBuffer & operator=(LinkedBuffer &);

    public:
        buffer::Buffer * get_current_buffer();

        void free_current_buffer();

        buffer::Buffer * allocate_buffer(std::size_t sz = 8192);

        void append_buffer(buffer::Buffer *buf);

        buffer::Buffer * take_current_buffer();

        std::size_t get_size() const 
        {
            return buffers_.size();
        }

        void clear();

        void free_all_buffers();

        void foreach(std::function<bool (buffer::Buffer *buff)> func);

        void keep_one_buffer();

        std::vector<buffer::Buffer *> to_vector(bool clear = false);

    private:
        std::list<buffer::Buffer *> buffers_;
    };
}

#endif
