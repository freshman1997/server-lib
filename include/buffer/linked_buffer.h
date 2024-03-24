#ifndef __LINKED_BUFF_H__
#define __LINKED_BUFF_H__
#include <list>

class Buffer;
class LinkedBuffer
{
public:
    LinkedBuffer();
    ~LinkedBuffer();

public:
    Buffer * get_current_buffer();

    void free_current_buffer();

    Buffer * allocate_buffer(std::size_t sz = 8192);

    void append_buffer(Buffer *buf);

    std::size_t get_size() const 
    {
        return buffers_.size();
    }

private:
    std::list<Buffer *> buffers_;
};

#endif
