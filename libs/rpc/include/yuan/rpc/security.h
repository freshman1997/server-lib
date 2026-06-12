#ifndef YUAN_RPC_SECURITY_H
#define YUAN_RPC_SECURITY_H

#include "wire.h"

#include <cstdint>
#include <string>

namespace yuan::rpc::security
{
    class XorStreamCipher
    {
    public:
        explicit XorStreamCipher(std::string key);

        bool operator()(const wire::CryptoContext &context, const Bytes &input, Bytes &output) const;

    private:
        std::string key_;
    };
}

#endif
