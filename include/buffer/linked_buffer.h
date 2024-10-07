#ifndef __LINKED_BUFF_H__
#define __LINKED_BUFF_H__
#include <functional>
#include <list>

class Buffer;
class LinkedBuffer
{
public:
    LinkedBuffer();
    ~LinkedBuffer();

public:
    LinkedBuffer & operator=(LinkedBuffer &);

public:
    Buffer * get_current_buffer();

    void free_current_buffer(Buffer *replaceBuff = nullptr);

    Buffer * allocate_buffer(std::size_t sz = 8192);

    void append_buffer(Buffer *buf);

    Buffer * take_current_buffer();

    std::size_t get_size() const 
    {
        return buffers_.size();
    }

    void clear();

    void free_all_buffers();

    void foreach(std::function<bool (Buffer *buff)> func);

    void keep_one_buffer();

private:
    std::list<Buffer *> buffers_;
};

#endif
