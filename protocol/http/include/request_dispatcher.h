#ifndef __HTTP_REQUEST_DISPATCHER_H__
#define __HTTP_REQUEST_DISPATCHER_H__
#include <string>
#include <string_view>
#include <unordered_map>

#include "common.h"
#include "base/utils/compressed_trie.h"

namespace yuan::net::http 
{
    class HttpRequestDispatcher
    {
        struct StringViewHash
        {
            using is_transparent = void;

            std::size_t operator()(std::string_view value) const noexcept
            {
                return std::hash<std::string_view>{}(value);
            }

            std::size_t operator()(const std::string &value) const noexcept
            {
                return std::hash<std::string_view>{}(value);
            }

            std::size_t operator()(const char *value) const noexcept
            {
                return value ? std::hash<std::string_view>{}(value) : 0;
            }
        };

        struct StringViewEq
        {
            using is_transparent = void;

            bool operator()(std::string_view lhs, std::string_view rhs) const noexcept
            {
                return lhs == rhs;
            }
        };

    public:
        void register_handler(const std::string &url, request_function func, bool is_prefix = false);

        request_function get_handler(const std::string &url) const;
        request_function get_handler(std::string_view url) const;

        const request_function *get_handler_ptr(const std::string &url) const;
        const request_function *get_handler_ptr(std::string_view url) const;

        const base::CompressTrie & get_compress_trie() const
        {
            return compress_trie_;
        }

    private:
        std::unordered_map<std::string, request_function, StringViewHash, StringViewEq> mappings_;
        base::CompressTrie compress_trie_;
    };
}


#endif
