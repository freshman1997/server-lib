#include "SearchHelper.h"

TrieNode::~TrieNode() 
{
    for (auto& it : children) 
    {
        delete it.second;
    }
}

TrieTree::TrieTree() : root(new TrieNode()), enableGap(false)
{}

TrieTree::~TrieTree() 
{ 
    delete root; 
}

void TrieTree::insert(const std::string& word, uint64_t gid) 
{
    insert_helper(root, word, gid);
}

std::set<SearchResult> TrieTree::fuzzy_search(const std::string& query) 
{
    std::set<SearchResult> results;

    const std::vector<std::string> &queryWords = tokenize(query);

    // dfs
    search(results, root, queryWords, 0, "", max_score);

    return results;
}

void TrieTree::remove(const std::string& word)
{
    const auto &words = tokenize(word);
    remove_helper(root, 0, words);
}

void TrieTree::update(uint64_t gid, const std::string& word)
{
    const auto &oldWord = find_word(gid);
    if (!oldWord.empty())
    {
        remove(oldWord);
    }
    insert(word, gid);
}

void TrieTree::insert_helper(TrieNode* node, const std::string& word, uint64_t gid) 
{
    const auto &words = tokenize(word);
    for (int i = 0; i < words.size(); ++i) 
    {
        const std::string &ch = words[i];
        if (node->children.find(ch) == node->children.end()) 
        {
            node->children[ch] = new TrieNode;
        }
        node = node->children[ch];
    }
    node->gid_set_.insert(gid);
}

void TrieTree::find_word(TrieNode* node, std::set<SearchResult> &results, const std::string &prefix, int baseScore)
{
    for (const auto& it : node->children) 
    {
        TrieNode* child = it.second;
        if (!child->gid_set_.empty()) 
        {
            const auto &tmpStr = prefix + it.first;
            for (auto &gid : child->gid_set_)
            {
                results.insert({gid, baseScore - 1, tmpStr});
            }
        }

        if (!child->children.empty()) 
        {
            find_word(child, results, prefix + it.first, baseScore - 1);
        }

        if (results.size() >= 20)
        {
            return;
        }
    }
}

void TrieTree::search(std::set<SearchResult> &results, TrieNode* node, const std::vector<std::string> &queryWords, int idx, const std::string &prefix, int baseScore)
{
    if (idx >= queryWords.size()) 
    {
        if (!node->gid_set_.empty()) 
        {
            for (auto &gid : node->gid_set_)
            {
                results.insert({gid, baseScore, prefix});
            }

            if (results.size() >= 20) 
            {
                return;
            }
        }
        
        find_word(node, results, prefix, baseScore);
        return;
    }

    const std::string &ch = queryWords[idx];
    for (const auto& it : node->children) 
    {
        const std::string &child_prefix = it.first;
        TrieNode* child = it.second;

        if (child_prefix == ch) 
        {
            search(results, child, queryWords, idx + 1, prefix + child_prefix, baseScore);
        } 
        else 
        {
            search(results, child, queryWords, enableGap ? idx : 0, prefix + child_prefix, baseScore - 1);
        }
    }
}

void TrieTree::remove_helper(TrieNode* node, int idx, const std::vector<std::string> &words)
{
    if (idx >= words.size()) 
    {
        node->gid_set_.clear();
        return;
    }

    const std::string &ch = words[idx];
    if (node->children.find(ch) == node->children.end()) 
    {
        return;
    }

    TrieNode* child = node->children[ch];
    remove_helper(child, idx + 1, words);

    if (child->children.empty() && child->gid_set_.empty()) 
    {
        node->children.erase(ch);
        delete child;
    }
}

void TrieTree::do_find_word(uint64_t gid, std::string &word, TrieNode* node, std::string prefix)
{
    if (node->gid_set_.find(gid) != node->gid_set_.end()) 
    {
        word = prefix;
        return;
    }

    for (const auto& it : node->children) 
    {
        do_find_word(gid, word, it.second, prefix + it.first);
    }
}

std::string TrieTree::find_word(uint64_t gid)
{
    std::string word;
    do_find_word(gid, word, root, "");
    return word;
}

std::string TrieTree::get_utf8_char(const std::string& str, int from) 
{
    if (str.empty()) 
    {
        return "";
    }

    int len = 1;
    if ((str[from] & 0xF0) == 0xF0) 
    {
        len = 4;
    }
    else if ((str[from] & 0xE0) == 0xE0) 
    {
        len = 3;
    }
    else if ((str[from] & 0xC0) == 0xC0) 
    {
        len = 2;
    }

    if (from + len > str.size()) 
    {
        return "";
    }

    return str.substr(from, len);
}

std::vector<std::string> TrieTree::tokenize(const std::string& str)
{
    std::vector<std::string> res;
    for (int i = 0; i < str.size(); ) 
    {
        const std::string &word = get_utf8_char(str, i);
        res.push_back(word);
        i += word.size();
    }
    return res;
}