#ifndef COMPRESS_TRIE_H_
#define COMPRESS_TRIE_H_

#include <cassert>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace yuan::base
{

    /*
     * Radix Tree (压缩前缀树 / PATRICIA trie)
     *
     * 与普通Trie的区别：
     *   - 普通 Trie: 每条边存1个字符 → O(N) 层深，N=字符串长度
     *   - Radix Tree: 每条边存储字符串 → 无单分支节点，层数大幅减少
     *
     * 典型场景：URL路由前缀匹配、IP路由查找等
     *
     * 示例：
     *   insert("/api/user",  is_prefix=true)
     *   insert("/api/order", is_prefix=true)
     *   insert("/static",    is_prefix=false)
     *
     *   find_prefix("/api/user/123") -> 返回 {match_len=9, is_registered_prefix=true}
     *   find_prefix("/static/js")    -> 返回 {match_len=7, is_registered_prefix=false}
     */

    class CompressTrie
    {
        // ============================================================
        // 内部节点：Radix树的每个节点
        // ============================================================
        struct Node
        {
            std::string edge;                       // 边上的标签字符串（非空）
            bool is_terminal      = false;           // 此节点是否是一个完整插入的单词终点
            bool is_prefix_marked = false;           // 此节点是否被显式标记为前缀匹配点

            std::vector<std::unique_ptr<Node>> children;  // 有序子节点列表

            // 禁止拷贝（含unique_ptr），允许移动
            Node() = default;
            ~Node() = default;

            Node(const Node&) = delete;
            Node& operator=(const Node&) = delete;
            Node(Node&&) noexcept = default;
            Node& operator=(Node&&) noexcept = default;
        };

        // ============================================================
        // 查找结果
        // ============================================================
    public:
        struct MatchResult
        {
            int match_length = 0;       // 匹配到的输入字符串长度（>0表示有匹配）
            bool is_registered = false;  // 匹配位置是否是被 insert(..., is_prefix=true) 标记过的

            explicit operator bool() const noexcept { return match_length > 0; }
        };

    public:
        CompressTrie() : root_(std::make_unique<Node>()) {}
        ~CompressTrie() = default;

        // 禁用拷贝（避免深层拷贝整棵树），允许移动
        CompressTrie(const CompressTrie&) = delete;
        CompressTrie& operator=(const CompressTrie&) = delete;
        CompressTrie(CompressTrie&&) noexcept = default;
        CompressTrie& operator=(CompressTrie&&) noexcept = default;

    public:
        // --------------------------------------------------------
        // 插入一个字符串
        // @param word      要插入的关键字（如 "/api/"）
        // @param as_prefix 是否将此位置标记为"前缀匹配点"
        //                   true  → find_prefix 时此位置会返回 is_registered=true
        //                   false → 只有精确匹配到此位置才算命中
        // --------------------------------------------------------
        void insert(const std::string &word, bool as_prefix = false);

        // --------------------------------------------------------
        // 精确查找：word是否作为完整关键字存在
        // --------------------------------------------------------
        bool contains(const std::string &word) const;

        // --------------------------------------------------------
        // 前缀查找：在trie中寻找与 word 的最长公共前缀
        //
        // 返回 MatchResult:
        //   .match_length > 0  → 匹配到的长度
        //   .is_registered     → 该匹配点是否是之前 insert 标记为 prefix 的位置
        //
        // 典型用法（代理路由）：
        //   auto r = trie.find_prefix(url);
        //   if (r && r.is_registered) {
        //       std::string matched = url.substr(0, r.match_length);  // 获取匹配的前缀
        //       // 执行代理转发...
        //   }
        // --------------------------------------------------------
        MatchResult find_prefix(const std::string &word) const;

        // --------------------------------------------------------
        // 便捷方法：是否有以 word 为前缀的关键字
        // --------------------------------------------------------
        bool has_key_with_prefix(const std::string &word) const;

        void clear();

        // 统计信息
        size_t size() const { return size_; }
        size_t node_count() const { return node_count_; }

    private:
        // 插入的内部递归实现
        void do_insert(Node *parent, std::string_view remaining, bool at_end_is_terminal, bool mark_as_prefix);

        // 前缀查找内部实现（零拷贝遍历）
        static MatchResult do_find_prefix(const Node *node, std::string_view remaining);

    private:
        std::unique_ptr<Node> root_;
        size_t size_       = 0;  // 已插入的不同关键字数量
        size_t node_count_ = 1;  // 总节点数（root始终算1）
    };

} // namespace yuan::base

#endif
