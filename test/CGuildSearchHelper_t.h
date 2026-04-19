
#ifndef __CGUILDSEARCHHELPER_T_H__
#define __CGUILDSEARCHHELPER_T_H__
#include <cstdint>
#include <string>
#include <unordered_map>
#include <set>
#include <vector>

constexpr int max_score = 64;

struct SearchResult
{
    uint64_t gid;
    int score;
    std::string word;

    SearchResult(uint64_t gid, int score, const std::string &w) : gid(gid), score(score), word(w) {}

    bool operator<(const SearchResult &rhs) const
    {
        return score >= rhs.score;
    }
};

struct TrieNode 
{
    std::unordered_map<std::string, TrieNode*> children; // 子节点（按 UTF-8 字符索引）
    std::set<uint64_t> gids; // 该节点对应的 gid 集合

    TrieNode() = default;
    ~TrieNode();
};

class TrieTree 
{
public:
    TrieTree();
    ~TrieTree();

    void insert(const std::string& word, uint64_t gid);

    std::set<SearchResult> fuzzy_search(const std::string& query);

    void remove(const std::string& word);

    void update(uint64_t gid, const std::string& word);

    void set_enable_gap(bool enableGap) { this->enableGap = enableGap; }

private:
    void insert_helper(TrieNode* node, const std::string& word, uint64_t gid);

    void find_word(TrieNode* node, std::set<SearchResult> &results, const std::string &prefix, int baseScore);

    void search(std::set<SearchResult> &results, TrieNode* node, const std::vector<std::string> &queryWords, int idx, const std::string &prefix, int baseScore);

    void remove_helper(TrieNode* node, int idx, const std::vector<std::string> &words);

    void do_find_word(uint64_t gid, std::string &word, TrieNode* node, std::string prefix);

    std::string find_word(uint64_t gid);

    std::string get_utf8_char(const std::string& str, int from);

    std::vector<std::string> tokenize(const std::string& str);

private:
    TrieNode* root;
    bool enableGap;
    std::size_t usedMemory = 0;
};

#endif // __CGUILDSEARCHHELPER_T_H__
