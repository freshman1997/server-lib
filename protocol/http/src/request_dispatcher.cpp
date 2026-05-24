#include "request_dispatcher.h"

namespace yuan::net::http
{
    void HttpRequestDispatcher::register_handler(const std::string &url, request_function func, bool is_prefix)
    {
        mappings_[url] = std::move(func);
        compress_trie_.insert(url, is_prefix);
    }

    request_function HttpRequestDispatcher::get_handler(const std::string &url) const
    {
        return get_handler(std::string_view(url));
    }

    request_function HttpRequestDispatcher::get_handler(std::string_view url) const
    {
        const auto *handler = get_handler_ptr(url);
        return handler ? *handler : nullptr;
    }

    const request_function *HttpRequestDispatcher::get_handler_ptr(const std::string &url) const
    {
        return get_handler_ptr(std::string_view(url));
    }

    const request_function *HttpRequestDispatcher::get_handler_ptr(std::string_view url) const
    {
        const auto exact = mappings_.find(url);
        if (exact != mappings_.end()) {
            return &exact->second;
        }

        const auto result = compress_trie_.find_prefix(url);
        std::string_view lookup = url;
        if (result && result.is_registered) {
            lookup = url.substr(0, static_cast<std::size_t>(result.match_length));
        }

        const auto it = mappings_.find(lookup);
        return it == mappings_.end() ? nullptr : &it->second;
    }
}
