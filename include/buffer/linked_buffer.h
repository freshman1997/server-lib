#ifndef __LINKED_BUFF_H__
#define __LINKED_BUFF_H__
#include <list>
#include "buffer/buffer.h"

class LinkedBuffer
{
public:
    Buffer & allocBuffer()
    {
        if (buffers_.empty()) {
            buffers_.push_back(Buffer());
        }

        return buffers_.back();
    }

    

private:
    std::list<Buffer> buffers_;
};

#endif
