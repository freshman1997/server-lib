#include "request_dispatcher.h"

namespace yuan::net::http 
{
    void HttpRequestDispatcher::register_handler(const std::string &url, request_function func, bool is_prefix)
    {
        mappings_[url] = func;
        compress_trie_.insert(url, is_prefix);
    }

    request_function HttpRequestDispatcher::get_handler(const std::string &url) const
    {
        const auto exact = mappings_.find(url);
        if (exact != mappings_.end()) {
            return exact->second;
        }

        auto result = compress_trie_.find_prefix(url);
        std::string prefix;
        
        // is_registered=true 表示匹配到被 insert(..., is_prefix=true) 标记过的前缀节点
        if (result && result.is_registered) {
            prefix = url.substr(0, static_cast<size_t>(result.match_length));
        }

        auto it = mappings_.find(prefix.empty() ? url : prefix);
        return it == mappings_.end() ? nullptr : it->second;
    }
}
