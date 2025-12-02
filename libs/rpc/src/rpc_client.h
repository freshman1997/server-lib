#ifndef ___YAUN_RPC_CLIENT_H__
#define ___YAUN_RPC_CLIENT_H__

#include <cstdint>
#include <string>
namespace yuan::rpc 
{
    class RpcClient 
    {
    public:
        RpcClient();
        ~RpcClient();

    public:
        
    private:
        void connect(const std::string &host, uint16_t port);
    };
}

#endif // ___YAUN_RPC_CLIENT_H__
