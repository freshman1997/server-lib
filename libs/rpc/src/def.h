#ifndef __DEF_H__
#define __DEF_H__

#include <cstdint>
namespace yuan::rpc 
{
    enum class RpcErrorCode : uint8_t
    {
        SUCCESS = 0,
        TIMEOUT = 1,
        NETWORK_ERROR = 2,
        SERVER_ERROR = 3,
        UNKNOWN_ERROR = 255
    };

    enum class RpcType : uint8_t
    {
        REQUEST = 0,
        RESPONSE = 1
    };

    enum class RpcStatus : uint8_t
    {
        SUCCESS = 0,
        FAILED = 1,
        TIMEOUT = 2,
        CANCEL = 3
    };
}

#endif // __DEF_H__
