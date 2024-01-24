#ifndef __HTTP_REQUEST_DISPATCHER_H__
#define __HTTP_REQUEST_DISPATCHER_H__
#include <string>
#include <unordered_map>

#include "../common.h"
#include "base/compressed_trie.h"

namespace net::http 
{
    class HttpRequestDispatcher
    {
    public:
        void register_handler(const std::string &url, request_function func);

        request_function get_handler(const std::string &url) const;

    private:

    private:
        std::unordered_map<std::string, request_function> mappings_;
        base::CompressTrie compress_trie_;
    };
}


#endif