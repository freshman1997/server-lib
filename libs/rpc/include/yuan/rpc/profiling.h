#ifndef YUAN_RPC_PROFILING_H
#define YUAN_RPC_PROFILING_H

#if defined(YUAN_ENABLE_TRACY)
#include <tracy/Tracy.hpp>
#define YUAN_RPC_PROFILE_ZONE(name) ZoneScopedN(name)
#else
#define YUAN_RPC_PROFILE_ZONE(name) ((void)0)
#endif

#endif
