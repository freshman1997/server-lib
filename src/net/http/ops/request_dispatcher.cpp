#include "net/http/ops/request_dispatcher.h"

namespace net::http 
{
    void HttpRequestDispatcher::register_handler(const std::string &url, request_function func)
    {
        if (compress_trie_.contains(url)) {
            // TODO warning
            return;
        }

        auto it = mappings_.find(url);
        if (it != mappings_.end()) {
            // TODO 
        }

        mappings_[url] = func;
        compress_trie_.insert(url);
    }

    request_function HttpRequestDispatcher::get_handler(const std::string &url) const
    {
        auto it = mappings_.find(url);
        return it == mappings_.end() ? nullptr : it->second;
    }
}