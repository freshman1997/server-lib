#ifndef __COMPRESS_TRIE_H__
#define __COMPRESS_TRIE_H__
#include <string>
#include <unordered_map>

namespace base
{
    /*
     * @date 2024-01-22
     * 压缩前缀树
     */
    class CompressTrie
    {
        struct Node {
            bool is_word = false;
            std::unordered_map<char, Node *> children;
        };

    public:
        CompressTrie();
        ~CompressTrie();

    public:
        void insert(const std::string &word);

        bool contains(const std::string &word);

        bool start_with(const std::string &word);

    private:
        Node *doInsert(Node *node, char ch, bool is_word);
        void free(Node *node);

    private:
        Node *root;
    };
}

#endif