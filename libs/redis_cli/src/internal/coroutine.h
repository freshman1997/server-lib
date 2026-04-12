#ifndef __YUAN_REDIS_COROUTINE_H__
#define __YUAN_REDIS_COROUTINE_H__

#include "coroutine/task.h"

namespace yuan::redis
{

template <typename T>
using SimpleTask = yuan::coroutine::Task<T>;

} // namespace yuan::redis

#endif // __YUAN_REDIS_COROUTINE_H__
