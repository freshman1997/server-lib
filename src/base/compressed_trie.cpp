#include "base/compressed_trie.h"

namespace base
{
    CompressTrie::CompressTrie() : root(nullptr) {}

    CompressTrie::~CompressTrie()
    {
        if (root) {
            free(root);
        }
    }

    void CompressTrie::insert(const std::string &word)
    {
        if (word.empty()) {
            return;
        }

        if (!root) {
            root = new Node;
        }

        Node *node = root;
        for (int i = 0; i < word.size(); ++i) {
            node = doInsert(node, word[i], i == word.size() - 1);
        }
    }

    bool CompressTrie::contains(const std::string &word)
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

    bool CompressTrie::start_with(const std::string &word)
    {
        if (!root || word.empty()) {
            return false;
        }

        int i = 0;
        for (Node *node = root; i < word.size(); ++i) {
            auto it = node->children.find(word[i]);
            if (it == node->children.end()) {
                break;
            }

            node = it->second;
        }

        return i >= word.size();
    }

    CompressTrie::Node *CompressTrie::doInsert(Node *node, char ch, bool is_word)
    {
        auto it = node->children.find(ch);
        if (it != node->children.end()) {
            Node *child = it->second;
            child->is_word = is_word;
            return child;
        }

        Node *newNode = new Node;
        newNode->is_word = is_word;
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