#ifndef __COROUTINE_HELPER_H__
#define __COROUTINE_HELPER_H__

#include "coroutine/task.h"

namespace yuan::rpc
{

template <typename T>
using CoroutineHelper = yuan::coroutine::Task<T>;

} // namespace yuan::rpc

#endif // __COROUTINE_HELPER_H__
