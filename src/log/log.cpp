#include "log/log.h"

namespace mlog
{
    Logger::Logger()
    {
        buff_.resize(DEFAULT_BUFFER_SIZE);
    }

    Logger::~Logger()
    {

    }
}