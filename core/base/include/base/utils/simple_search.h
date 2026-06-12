#ifndef YUAN_BASE_UTILS_SIMPLE_SEARCH_H_
#define YUAN_BASE_UTILS_SIMPLE_SEARCH_H_

#include <algorithm>
#include <cstddef>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace yuan::base
{

    // SimpleSearch 是一个轻量级内存搜索索引，适合中小规模的 UTF-8 短文本集合。
    // 典型场景包括：公会名、用户昵称、房间名、命令名、配置 key、文件名、标签、
    // 短标题等需要快速本地检索的功能。
    //
    // 搜索模式：
    // - Fuzzy：基于 Trie 的 DFS 模糊搜索，会优先走精确匹配分支。适合昵称、
    //   公会名、房间名这类短文本模糊查找。调用 set_enable_gap(true) 后允许跳过
    //   被索引文本中的字符，例如 query "ab" 可以匹配 text "axb"。
    // - Prefix：高效 Trie 前缀搜索。适合命令、路径、配置 key，以及只希望匹配
    //   开头部分的名称搜索。
    // - Contains：UTF-8 bigram 倒排索引 + std::string::find 最终确认。适合标签、
    //   文件名、标题、配置 value 等短文本片段搜索。单 token 查询会退化为线性扫描，
    //   因为单字符/单 token 通常区分度不高。
    //
    // 基本用法：
    //   yuan::base::SimpleSearch<uint64_t> search;
    //   search.insert(1001, "dragon");
    //   search.update(1001, "dragonfly");
    //   auto fuzzy = search.search("drag", 20);
    //   auto prefix = search.prefix_search("drag", 20);
    //   auto contains = search.contains_search("gon", 20);
    //   auto all = search.search("drag", 0); // limit 为 0 表示搜完整个索引。
    //   search.remove(1001);
    //
    // 注意：
    // - 搜索结果只包含 id 和 score，不复制 text；调用方通常可以通过 id 从业务数据
    //   中取回完整对象。需要查看索引内文本时，可以使用 find_text(id)。
    // - Id 需要能作为 std::unordered_map 的 key，并且支持 operator<，用于结果同分
    //   时的稳定排序。
    // - 文本按 UTF-8 字符切分，不做自然语言分词。这个类不提供拼音、错别字纠错、
    //   复杂相关性排序，也不适合作为大规模全文检索引擎。
    template <typename Id>
    struct SimpleSearchResult
    {
        Id id{};
        int score = 0;

        bool operator<(const SimpleSearchResult &rhs) const
        {
            if (score != rhs.score) {
                return score > rhs.score;
            }
            if (id != rhs.id) {
                return id < rhs.id;
            }
            return false;
        }
    };

    template <typename Id>
    class SimpleSearch
    {
    public:
        using IdType = Id;
        using Result = SimpleSearchResult<Id>;
        using ResultSet = std::set<Result>;

        enum class Mode
        {
            Fuzzy,
            Prefix,
            Contains,
        };

        static constexpr int max_score = 64;
        static constexpr std::size_t default_limit = 20;
        static constexpr std::size_t contains_ngram_size = 2;

    private:
        struct Node
        {
            std::unordered_map<std::string, std::unique_ptr<Node>> children;
            std::set<Id> ids;
        };

    public:
        SimpleSearch() = default;
        ~SimpleSearch() = default;

        SimpleSearch(const SimpleSearch&) = delete;
        SimpleSearch& operator=(const SimpleSearch&) = delete;
        SimpleSearch(SimpleSearch&&) noexcept = default;
        SimpleSearch& operator=(SimpleSearch&&) noexcept = default;

        void insert(const Id &id, const std::string &text)
        {
            remove(id);

            Node *node = &root_;
            const auto tokens = tokenize(text);
            for (const auto &token : tokens) {
                auto &child = node->children[token];
                if (!child) {
                    child = std::make_unique<Node>();
                }
                node = child.get();
            }

            node->ids.insert(id);
            id_text_[id] = text;
            add_to_contains_index(id, tokens);
        }

        void update(const Id &id, const std::string &text)
        {
            insert(id, text);
        }

        void remove(const Id &id)
        {
            const auto it = id_text_.find(id);
            if (it == id_text_.end()) {
                return;
            }

            const auto tokens = tokenize(it->second);
            remove_node(&root_, tokens, 0, id);
            remove_from_contains_index(id, tokens);
            id_text_.erase(it);
        }

        void remove_text(const std::string &text)
        {
            const auto tokens = tokenize(text);
            auto it = id_text_.begin();
            while (it != id_text_.end()) {
                if (it->second == text) {
                    remove_node(&root_, tokens, 0, it->first);
                    remove_from_contains_index(it->first, tokens);
                    it = id_text_.erase(it);
                    continue;
                }
                ++it;
            }
        }

        ResultSet search(const std::string &query) const
        {
            return search(query, default_limit);
        }

        ResultSet search(const std::string &query, std::size_t limit) const
        {
            return fuzzy_search(query, limit);
        }

        ResultSet search(const std::string &query, std::size_t limit, Mode mode) const
        {
            switch (mode) {
            case Mode::Prefix:
                return prefix_search(query, limit);
            case Mode::Contains:
                return contains_search(query, limit);
            case Mode::Fuzzy:
            default:
                return fuzzy_search(query, limit);
            }
        }

        ResultSet fuzzy_search(const std::string &query) const
        {
            return fuzzy_search(query, default_limit);
        }

        ResultSet fuzzy_search(const std::string &query, std::size_t limit) const
        {
            ResultSet results;
            std::string prefix;
            const auto tokens = tokenize(query);
            search_node(results, &root_, tokens, 0, prefix, max_score, limit);
            return results;
        }

        ResultSet prefix_search(const std::string &query) const
        {
            return prefix_search(query, default_limit);
        }

        ResultSet prefix_search(const std::string &query, std::size_t limit) const
        {
            ResultSet results;
            std::string prefix;
            const Node *node = find_prefix_node(query, prefix);
            if (!node) {
                return results;
            }

            for (const auto &id : node->ids) {
                results.insert(Result{ id, max_score });
                if (reached_limit(results, limit)) {
                    return results;
                }
            }

            collect_words(results, node, prefix, max_score, limit);
            return results;
        }

        ResultSet contains_search(const std::string &query) const
        {
            return contains_search(query, default_limit);
        }

        ResultSet contains_search(const std::string &query, std::size_t limit) const
        {
            ResultSet results;
            if (query.empty()) {
                collect_all_texts(results, limit);
                return results;
            }

            const auto query_tokens = tokenize(query);
            const std::set<Id> *candidates = select_contains_candidates(query_tokens);
            if (!candidates && query_tokens.size() >= contains_ngram_size) {
                return results;
            }

            const auto scan_candidate = [&](const Id &id) {
                const auto text_it = id_text_.find(id);
                if (text_it == id_text_.end()) {
                    return false;
                }

                const auto pos = text_it->second.find(query);
                if (pos == std::string::npos) {
                    return false;
                }

                int score = max_score;
                if (pos != 0) {
                    const auto penalty = static_cast<int>(std::min<std::size_t>(pos, max_score));
                    score -= penalty;
                }

                results.insert(Result{ id, score });
                return reached_limit(results, limit);
            };

            if (candidates) {
                for (const auto &id : *candidates) {
                    if (scan_candidate(id)) {
                        return results;
                    }
                }
            } else {
                for (const auto &item : id_text_) {
                    if (scan_candidate(item.first)) {
                        return results;
                    }
                }
            }

            return results;
        }

        void clear()
        {
            root_ = Node{};
            id_text_.clear();
            gram_index_.clear();
        }

        void set_enable_gap(bool enable)
        {
            enable_gap_ = enable;
        }

        bool enable_gap() const
        {
            return enable_gap_;
        }

        std::size_t size() const
        {
            return id_text_.size();
        }

        bool empty() const
        {
            return id_text_.empty();
        }

        const std::string* find_text(const Id &id) const
        {
            const auto it = id_text_.find(id);
            if (it == id_text_.end()) {
                return nullptr;
            }
            return &it->second;
        }

    private:
        static std::string utf8_char(const std::string &text, std::size_t from)
        {
            if (text.empty() || from >= text.size()) {
                return {};
            }

            const unsigned char ch = static_cast<unsigned char>(text[from]);
            std::size_t len = 1;
            if ((ch & 0xF8) == 0xF0) {
                len = 4;
            } else if ((ch & 0xF0) == 0xE0) {
                len = 3;
            } else if ((ch & 0xE0) == 0xC0) {
                len = 2;
            }

            if (from + len > text.size()) {
                return {};
            }

            return text.substr(from, len);
        }

        static std::vector<std::string> tokenize(const std::string &text)
        {
            std::vector<std::string> tokens;
            for (std::size_t i = 0; i < text.size();) {
                const auto token = utf8_char(text, i);
                if (token.empty()) {
                    ++i;
                    continue;
                }

                tokens.push_back(token);
                i += token.size();
            }
            return tokens;
        }

        static std::string make_ngram(const std::vector<std::string> &tokens, std::size_t from)
        {
            std::string gram;
            for (std::size_t i = 0; i < contains_ngram_size; ++i) {
                gram += tokens[from + i];
            }
            return gram;
        }

        void add_to_contains_index(const Id &id, const std::vector<std::string> &tokens)
        {
            if (tokens.size() < contains_ngram_size) {
                return;
            }

            for (std::size_t i = 0; i + contains_ngram_size <= tokens.size(); ++i) {
                gram_index_[make_ngram(tokens, i)].insert(id);
            }
        }

        void remove_from_contains_index(const Id &id, const std::vector<std::string> &tokens)
        {
            if (tokens.size() < contains_ngram_size) {
                return;
            }

            for (std::size_t i = 0; i + contains_ngram_size <= tokens.size(); ++i) {
                const auto gram = make_ngram(tokens, i);
                auto it = gram_index_.find(gram);
                if (it == gram_index_.end()) {
                    continue;
                }

                it->second.erase(id);
                if (it->second.empty()) {
                    gram_index_.erase(it);
                }
            }
        }

        const std::set<Id>* select_contains_candidates(const std::vector<std::string> &query_tokens) const
        {
            if (query_tokens.size() < contains_ngram_size) {
                return nullptr;
            }

            const std::set<Id> *best = nullptr;
            for (std::size_t i = 0; i + contains_ngram_size <= query_tokens.size(); ++i) {
                const auto gram = make_ngram(query_tokens, i);
                const auto it = gram_index_.find(gram);
                if (it == gram_index_.end()) {
                    return nullptr;
                }

                if (!best || it->second.size() < best->size()) {
                    best = &it->second;
                }
            }

            return best;
        }

        bool collect_words(ResultSet &results, const Node *node, std::string &prefix, int base_score, std::size_t limit) const
        {
            for (const auto &item : node->children) {
                if (reached_limit(results, limit)) {
                    return true;
                }

                const auto old_size = prefix.size();
                prefix += item.first;
                const Node *child = item.second.get();

                for (const auto &id : child->ids) {
                    results.insert(Result{ id, base_score - 1 });
                    if (reached_limit(results, limit)) {
                        prefix.resize(old_size);
                        return true;
                    }
                }

                if (collect_words(results, child, prefix, base_score - 1, limit)) {
                    prefix.resize(old_size);
                    return true;
                }

                prefix.resize(old_size);
            }

            return false;
        }

        void collect_all_texts(ResultSet &results, std::size_t limit) const
        {
            for (const auto &item : id_text_) {
                results.insert(Result{ item.first, max_score });
                if (reached_limit(results, limit)) {
                    return;
                }
            }
        }

        const Node* find_prefix_node(const std::string &query, std::string &prefix) const
        {
            const Node *node = &root_;
            const auto tokens = tokenize(query);
            for (const auto &token : tokens) {
                const auto it = node->children.find(token);
                if (it == node->children.end()) {
                    return nullptr;
                }

                prefix += token;
                node = it->second.get();
            }

            return node;
        }

        bool search_node(ResultSet &results, const Node *node, const std::vector<std::string> &query,
                         std::size_t index, std::string &prefix, int base_score, std::size_t limit) const
        {
            if (reached_limit(results, limit)) {
                return true;
            }

            if (index >= query.size()) {
                for (const auto &id : node->ids) {
                    results.insert(Result{ id, base_score });
                    if (reached_limit(results, limit)) {
                        return true;
                    }
                }
                return collect_words(results, node, prefix, base_score, limit);
            }

            const auto &token = query[index];
            const auto exact = node->children.find(token);
            if (exact != node->children.end()) {
                if (search_child(results, exact->second.get(), query, index + 1, prefix, exact->first, base_score, limit)) {
                    return true;
                }
            }

            for (const auto &item : node->children) {
                if (reached_limit(results, limit)) {
                    return true;
                }
                if (item.first == token) {
                    continue;
                }

                if (search_child(results, item.second.get(), query, enable_gap_ ? index : 0,
                                 prefix, item.first, base_score - 1, limit)) {
                    return true;
                }
            }

            return false;
        }

        bool search_child(ResultSet &results, const Node *child, const std::vector<std::string> &query,
                          std::size_t index, std::string &prefix, const std::string &edge,
                          int base_score, std::size_t limit) const
        {
            const auto old_size = prefix.size();
            prefix += edge;
            const bool stopped = search_node(results, child, query, index, prefix, base_score, limit);
            prefix.resize(old_size);
            return stopped;
        }

        bool remove_node(Node *node, const std::vector<std::string> &tokens, std::size_t index, const Id &id)
        {
            if (index >= tokens.size()) {
                node->ids.erase(id);
                return node->children.empty() && node->ids.empty();
            }

            const auto it = node->children.find(tokens[index]);
            if (it == node->children.end()) {
                return node->children.empty() && node->ids.empty();
            }

            if (remove_node(it->second.get(), tokens, index + 1, id)) {
                node->children.erase(it);
            }

            return node->children.empty() && node->ids.empty();
        }

        static bool reached_limit(const ResultSet &results, std::size_t limit)
        {
            return limit > 0 && results.size() >= limit;
        }

    private:
        Node root_;
        bool enable_gap_ = false;
        std::unordered_map<Id, std::string> id_text_;
        std::unordered_map<std::string, std::set<Id>> gram_index_;
    };

} // namespace yuan::base

#endif // YUAN_BASE_UTILS_SIMPLE_SEARCH_H_
