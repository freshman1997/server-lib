#include "log/log.h"
#include "buffer/pool.h"

namespace yuan::log
{
    Logger::Logger()
    {
        buff_ = buffer::BufferedPool::get_instance()->allocate(DEFAULT_BUFFER_SIZE);
    }

    Logger::~Logger()
    {

    }
}