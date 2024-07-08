#ifndef COMPRESS_TRIE_H_
#define COMPRESS_TRIE_H_
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
            bool is_prefix = false;
            std::unordered_map<char, Node *> children;
        };

    public:
        CompressTrie();
        ~CompressTrie();

    public:
        void insert(const std::string &word, bool is_prefix = false);

        bool contains(const std::string &word) const;

        bool start_with(const std::string &word) const;

        int find_prefix(const std::string &word, bool check_prefix = false) const;

    private:
        Node *doInsert(Node *node, char ch, bool is_word, bool is_prefix);
        void free(Node *node);

    private:
        Node *root;
    };
}

#endif