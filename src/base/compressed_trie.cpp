#include "base/utils/compressed_trie.h"

namespace base
{
    CompressTrie::CompressTrie() : root(nullptr) {}

    CompressTrie::~CompressTrie()
    {
        if (root) {
            free(root);
        }
    }

    void CompressTrie::insert(const std::string &word, bool is_prefix)
    {
        if (word.empty()) {
            return;
        }

        if (!root) {
            root = new Node;
        }

        Node *node = root;
        for (int i = 0; i < word.size(); ++i) {
            node = doInsert(node, word[i], i == word.size() - 1, is_prefix);
        }
    }

    bool CompressTrie::contains(const std::string &word) const
    {
        if (!root) {
            return false;
        }

        Node *node = root;
        int i = 0;
        for (; i < word.size(); ++i) {
            auto it = node->children.find(word[i]);
            if (it == node->children.end()) {
                break;
            }

            node = it->second;
        }

        return node->is_word && i == word.size();
    }

    bool CompressTrie::start_with(const std::string &word) const
    {
        return find_prefix(word) >= word.size();
    }

    int CompressTrie::find_prefix(const std::string &word, bool check_prefix) const
    {
        if (!root || word.empty()) {
            return false;
        }

        int i = 0;
        Node *node = root;
        for (; i < word.size(); ++i) {
            auto it = node->children.find(word[i]);
            if (it == node->children.end()) {
                break;
            }

            node = it->second;
        }

        if (!check_prefix) {
            return i;
        }

        return node->is_prefix ? (i == 0 ? -1 : -i) : i;
    }

    CompressTrie::Node *CompressTrie::doInsert(Node *node, char ch, bool is_word, bool is_prefix)
    {
        auto it = node->children.find(ch);
        if (it != node->children.end()) {
            Node *child = it->second;
            child->is_word = is_word;
            return child;
        }

        Node *newNode = new Node;
        newNode->is_word = is_word;
        if (is_word) {
            newNode->is_prefix = is_prefix;
        }

        node->children[ch] = newNode;

        return newNode;
    }

    void CompressTrie::free(Node *node)
    {
        for (const auto &it : node->children) {
            free(it.second);
        }

        delete node;
    }

} // namespace base