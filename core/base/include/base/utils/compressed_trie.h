#ifndef COMPRESS_TRIE_H_
#define COMPRESS_TRIE_H_

#include <cassert>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace yuan::base
{

    class CompressTrie
    {
        struct Node
        {
            std::string edge;
            bool is_terminal = false;
            bool is_prefix_marked = false;
            std::vector<std::unique_ptr<Node>> children;

            Node() = default;
            ~Node() = default;

            Node(const Node&) = delete;
            Node& operator=(const Node&) = delete;
            Node(Node&&) noexcept = default;
            Node& operator=(Node&&) noexcept = default;
        };

    public:
        struct MatchResult
        {
            int match_length = 0;
            bool is_registered = false;
            bool is_terminal = false;

            explicit operator bool() const noexcept { return match_length > 0; }
        };

    public:
        CompressTrie() : root_(std::make_unique<Node>()) {}
        ~CompressTrie() = default;

        CompressTrie(const CompressTrie&) = delete;
        CompressTrie& operator=(const CompressTrie&) = delete;
        CompressTrie(CompressTrie&&) noexcept = default;
        CompressTrie& operator=(CompressTrie&&) noexcept = default;

        void insert(const std::string &word, bool as_prefix = false);
        bool contains(const std::string &word) const;
        bool contains(std::string_view word) const;

        // Returns an exact terminal match first. If no exact terminal matches,
        // returns the longest key explicitly registered as a prefix.
        MatchResult find_prefix(const std::string &word) const;
        MatchResult find_prefix(std::string_view word) const;

        bool has_key_with_prefix(const std::string &word) const;
        bool has_key_with_prefix(std::string_view word) const;
        void clear();

        size_t size() const { return size_; }
        size_t node_count() const { return node_count_; }

    private:
        void do_insert(Node *parent, std::string_view remaining, bool at_end_is_terminal, bool mark_as_prefix);
        static MatchResult do_find_prefix(const Node *node, std::string_view remaining);

    private:
        std::unique_ptr<Node> root_;
        size_t size_ = 0;
        size_t node_count_ = 1;
    };

} // namespace yuan::base

#endif
