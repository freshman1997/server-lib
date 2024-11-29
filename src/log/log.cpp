#include "log/log.h"
#include "buffer/pool.h"

namespace mlog
{
    Logger::Logger()
    {
        buff_ = BufferedPool::get_instance()->allocate(DEFAULT_BUFFER_SIZE);
    }

    Logger::~Logger()
    {

    }
}