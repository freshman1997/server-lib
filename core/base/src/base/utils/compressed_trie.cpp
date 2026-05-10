#include "base/utils/compressed_trie.h"

#include <algorithm>

namespace yuan::base
{
    namespace
    {
        size_t common_prefix_length(std::string_view lhs, std::string_view rhs) noexcept
        {
            const size_t max_len = std::min(lhs.size(), rhs.size());
            size_t common = 0;
            while (common < max_len && lhs[common] == rhs[common]) {
                ++common;
            }
            return common;
        }

        const CompressTrie::MatchResult no_match{};
    }

    void CompressTrie::insert(const std::string &word, bool as_prefix)
    {
        if (word.empty()) {
            return;
        }

        const bool already_present = contains(word);
        do_insert(root_.get(), std::string_view(word), true, as_prefix);
        if (!already_present) {
            ++size_;
        }
    }

    bool CompressTrie::contains(const std::string &word) const
    {
        if (word.empty()) {
            return false;
        }

        const auto result = do_find_prefix(root_.get(), std::string_view(word));
        return result.is_terminal && result.match_length == static_cast<int>(word.size());
    }

    CompressTrie::MatchResult CompressTrie::find_prefix(const std::string &word) const
    {
        if (word.empty() || !root_) {
            return no_match;
        }

        return do_find_prefix(root_.get(), std::string_view(word));
    }

    bool CompressTrie::has_key_with_prefix(const std::string &word) const
    {
        if (word.empty() || !root_) {
            return false;
        }

        const Node *node = root_.get();
        std::string_view remaining(word);

        while (!remaining.empty()) {
            const Node *next = nullptr;
            for (const auto &child : node->children) {
                if (child && !child->edge.empty() && child->edge.front() == remaining.front()) {
                    next = child.get();
                    break;
                }
            }

            if (!next) {
                return false;
            }

            const size_t common = common_prefix_length(next->edge, remaining);
            if (common == remaining.size()) {
                return true;
            }
            if (common < next->edge.size()) {
                return false;
            }

            node = next;
            remaining.remove_prefix(common);
        }

        return true;
    }

    void CompressTrie::clear()
    {
        root_ = std::make_unique<Node>();
        size_ = 0;
        node_count_ = 1;
    }

    void CompressTrie::do_insert(Node *parent, std::string_view remaining,
                                 bool at_end_is_terminal, bool mark_as_prefix)
    {
        assert(parent);
        if (remaining.empty()) {
            if (at_end_is_terminal) {
                parent->is_terminal = true;
            }
            if (mark_as_prefix) {
                parent->is_prefix_marked = true;
            }
            return;
        }

        for (size_t index = 0; index < parent->children.size(); ++index) {
            auto &child = parent->children[index];
            if (!child || child->edge.empty()) {
                continue;
            }

            const size_t common = common_prefix_length(child->edge, remaining);
            if (common == 0) {
                continue;
            }

            if (common == child->edge.size()) {
                const auto rest = remaining.substr(common);
                if (rest.empty()) {
                    if (at_end_is_terminal) {
                        child->is_terminal = true;
                    }
                    if (mark_as_prefix) {
                        child->is_prefix_marked = true;
                    }
                    return;
                }

                do_insert(child.get(), rest, at_end_is_terminal, mark_as_prefix);
                return;
            }

            auto split_node = std::make_unique<Node>();
            split_node->edge = child->edge.substr(0, common);
            child->edge.erase(0, common);
            split_node->children.push_back(std::move(child));

            if (common == remaining.size()) {
                split_node->is_terminal = at_end_is_terminal;
                split_node->is_prefix_marked = mark_as_prefix;
            } else {
                auto leaf = std::make_unique<Node>();
                leaf->edge = std::string(remaining.substr(common));
                leaf->is_terminal = at_end_is_terminal;
                leaf->is_prefix_marked = at_end_is_terminal && mark_as_prefix;
                split_node->children.push_back(std::move(leaf));
                ++node_count_;
            }

            parent->children[index] = std::move(split_node);
            ++node_count_;
            return;
        }

        auto leaf = std::make_unique<Node>();
        leaf->edge = std::string(remaining);
        leaf->is_terminal = at_end_is_terminal;
        leaf->is_prefix_marked = at_end_is_terminal && mark_as_prefix;
        parent->children.push_back(std::move(leaf));
        ++node_count_;
    }

    CompressTrie::MatchResult CompressTrie::do_find_prefix(const Node *node, std::string_view remaining)
    {
        if (!node || remaining.empty()) {
            return no_match;
        }

        MatchResult best_registered{};
        int total_matched = 0;

        while (!remaining.empty()) {
            const Node *next = nullptr;
            for (const auto &child : node->children) {
                if (child && !child->edge.empty() && child->edge.front() == remaining.front()) {
                    next = child.get();
                    break;
                }
            }

            if (!next) {
                return best_registered ? best_registered : MatchResult{ total_matched, false, false };
            }

            const size_t common = common_prefix_length(next->edge, remaining);
            if (common == 0) {
                return best_registered ? best_registered : MatchResult{ total_matched, false, false };
            }

            total_matched += static_cast<int>(common);

            if (common < next->edge.size()) {
                return best_registered ? best_registered : MatchResult{ total_matched, false, false };
            }

            if (next->is_prefix_marked) {
                best_registered = MatchResult{ total_matched, true, false };
            }

            if (common == remaining.size()) {
                if (next->is_terminal) {
                    return MatchResult{ total_matched, next->is_prefix_marked, true };
                }
                return best_registered ? best_registered : MatchResult{ total_matched, false, false };
            }

            node = next;
            remaining.remove_prefix(common);
        }

        return best_registered ? best_registered : MatchResult{ total_matched, false, false };
    }

} // namespace yuan::base
