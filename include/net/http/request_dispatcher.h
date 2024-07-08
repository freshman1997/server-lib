#ifndef __HTTP_REQUEST_DISPATCHER_H__
#define __HTTP_REQUEST_DISPATCHER_H__
#include <string>
#include <unordered_map>

#include "common.h"
#include "../../base/utils/compressed_trie.h"

namespace net::http 
{
    class HttpRequestDispatcher
    {
    public:
        void register_handler(const std::string &url, request_function func, bool is_prefix = false);

        request_function get_handler(const std::string &url) const;

        const base::CompressTrie & get_compress_trie() const
        {
            return compress_trie_;
        }

    private:
        std::unordered_map<std::string, request_function> mappings_;
        base::CompressTrie compress_trie_;
    };
}


#endif