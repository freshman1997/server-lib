#ifndef __LINKED_BUFF_H__
#define __LINKED_BUFF_H__
#include <list>
#include "buffer/buffer.h"

class LinkedBuffer
{
public:
    void traverse();

private:
    std::list<Buffer> buffers_;
};

#endif
