#include "request_dispatcher.h"

namespace net::http 
{
    void HttpRequestDispatcher::register_handler(const std::string &url, request_function func, bool is_prefix)
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
        compress_trie_.insert(url, is_prefix);
    }

    request_function HttpRequestDispatcher::get_handler(const std::string &url) const
    {
        int prefixIdx = compress_trie_.find_prefix(url, true);
        std::string prefix;
        if (prefixIdx < 0) {
            prefix = url.substr(0, -prefixIdx);
        }

        auto it = mappings_.find(prefix.empty() ? url : prefix);
        return it == mappings_.end() ? nullptr : it->second;
    }
}