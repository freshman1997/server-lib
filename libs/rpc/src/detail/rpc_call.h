#ifndef ___YAUN_RPC_CALL_H__
#define ___YAUN_RPC_CALL_H__

#include <cstdint>
#include <string>
namespace yuan::rpc 
{
    template<typename BodyType>
    class RpcCall
    {
    public:
        RpcCall(uint16_t serviceId, const std::string &method, BodyType &&body)
            : serviceId_(serviceId), method_(method), body_(std::move(body))
        {
        };

        ~RpcCall();

    private:
        uint16_t serviceId_;
        uint64_t requestId_;
        std::string method_;
        BodyType body_;
    };
}

#endif // ___YAUN_RPC_CALL_H__
