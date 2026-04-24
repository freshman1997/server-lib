#include "base/utils/compressed_trie.h"

namespace yuan::base
{

// ============================================================
// insert - 公共接口
// ============================================================

void CompressTrie::insert(const std::string &word, bool as_prefix)
{
    if (word.empty()) return;

    do_insert(&*root_, std::string_view(word), /*at_end=*/true, /*mark_prefix=*/as_prefix);
    ++size_;
}


// ============================================================
// contains - 精确查找
// ============================================================

bool CompressTrie::contains(const std::string &word) const
{
    if (word.empty()) return false;

    auto result = do_find_prefix(&*root_, std::string_view(word));
    // 精确匹配要求：遍历完整个 word 且 终点节点标记为 terminal
    return result.match_length == static_cast<int>(word.size());
}


// ============================================================
// find_prefix - 最长前缀匹配（核心操作）
// ============================================================

CompressTrie::MatchResult CompressTrie::find_prefix(const std::string &word) const
{
    if (word.empty() || !root_) {
        return MatchResult{0, false};
    }

    return do_find_prefix(&*root_, std::string_view(word));
}

bool CompressTrie::has_key_with_prefix(const std::string &word) const
{
    auto r = find_prefix(word);
    // 只要能完整走完 word 的所有字符，就说明存在以 word 为前缀的key
    return r.match_length >= static_cast<int>(word.size());
}

void CompressTrie::clear()
{
    root_ = std::make_unique<Node>();
    size_ = 0;
    node_count_ = 1;
}


// ============================================================
// do_insert - 内部递归插入（Radix Tree 核心算法）
//
// Radix Tree 的插入有3种情况：
//   1) 分裂 (split): 边标签部分匹配，需要拆分节点
//   2) 延伸 (extend): 当前边完全匹配，继续向子节点递归
//   3) 新增 (add new child): 无匹配边，直接添加新子节点
// ============================================================

void CompressTrie::do_insert(Node *parent, std::string_view remaining,
                              bool at_end_is_terminal, bool mark_as_prefix)
{
    assert(parent && !remaining.empty());

    for (auto &&child : parent->children)
    {
        const std::string &edge = child->edge;

        // 计算当前边与剩余输入的公共前缀长度
        size_t common = 0;
        size_t min_len = std::min(edge.size(), remaining.size());
        while (common < min_len && edge[common] == remaining[common])
        {
            ++common;
        }

        if (common == 0)
        {
            // 完全不匹配，检查下一个子节点
            continue;
        }

        if (common == edge.size())
        {
            // === 情况2: 边标签被完全消耗 ===
            // edge="api", remaining="api/user" → common=3=edge.size()
            // 沿着这条边继续向下递归

            auto rest = remaining.substr(common);  // "/user"

            if (rest.empty())
            {
                // 正好到此节点结束 → 更新此节点的标记
                if (at_end_is_terminal)
                    child->is_terminal = true;
                if (mark_as_prefix)
                    child->is_prefix_marked = true;
                return;
            }

            do_insert(&*child, rest, at_end_is_terminal, mark_as_prefix);
            return;
        }

        // === 情况1: 部分匹配，需要分裂 ===
        // 例如：已有边 "abcde"，现在插入 "abxyz"
        //       common=2 ("ab")
        //       需要分裂为:  "ab" -> ["cde"(旧), "xyz"(新)]

        // 1. 创建中间分裂节点
        auto split_node = std::make_unique<Node>();
        split_node->edge = edge.substr(0, common);  // "ab"

        // 2. 将原来的长边子节点缩短后挂到分裂节点下
        child->edge = edge.substr(common);           // "cde"
        split_node->children.push_back(std::move(child));

        // 3. 如果新插入的字符串也在此结束，设置分裂节点标记
        if (common == remaining.size())
        {
            // 新字符串正好在分裂节点结束
            if (at_end_is_terminal)
                split_node->is_terminal = true;
            if (mark_as_prefix)
                split_node->is_prefix_marked = true;
        }
        else
        {
            // 新字符串还有剩余部分，创建新的叶节点
            auto leaf = std::make_unique<Node>();
            leaf->edge = remaining.substr(common);   // "xyz"
            leaf->is_terminal = at_end_is_terminal;   // 叶子是终点
            if (leaf->is_terminal && mark_as_prefix)
                leaf->is_prefix_marked = true;
            split_node->children.push_back(std::move(leaf));
            ++node_count_;
        }

        // 4. 用分裂节点替换原来parent中的child
        // 注意：此时child已经被move走了，需要找到对应位置替换
        for (auto it = parent->children.begin(); it != parent->children.end(); ++it)
        {
            if (it->get() == nullptr || (*it)->edge.empty())
            {
                // 已被移走的位置（或占位），替换为split_node
                *it = std::move(split_node);
                break;
            }
            // 由于我们之前持有child引用，这里用另一种方式：
            // 实际上上面的循环逻辑不对，因为unique_ptr move后原位置变为null
            // 让我重新处理...
        }
        
        // 上面的循环方式不够健壮。更可靠的做法：
        // 因为child已被move到split_node->children中，原位置的unique_ptr已为空
        // 我们需要在parent->children中找到那个空位并替换
        // 但由于vector迭代器在move后不稳定，改用索引方式

        // 重新实现：先记录位置，再替换
        // （由于前面已经move了child，这里需要特殊处理）
        // 实际上最简单的方式是在进入分支时就记录位置
        
        // 为了代码清晰，采用以下策略：
        // 在发现部分匹配时，不立即修改parent->children，
        // 而是构建好split_node后统一替换。

        // 注意：由于上面已经执行了 `child->edge = edge.substr(common)`
        // 以及 `split_node->children.push_back(std::move(child))` 
        // 此时 parent->children 中的对应元素已经是一个空的 unique_ptr 了。
        // 我们需要把它替换掉。
        
        // 但实际上，因为我们在for-range循环中使用了 `auto &&child`
        // 这个引用指向的是 vector 中的元素，move之后它确实变成了 empty state。
        // 所以我们需要找到它并替换。

        // 这里用一个简单的方法：既然我们已经知道哪个位置变空了，
        // 直接扫描找空位替换即可。（生产环境可以用更好的数据结构）
        for (auto &c : parent->children)
        {
            if (!c)  // 找到被 move 掏空的 slot
            {
                c = std::move(split_node);
                break;
            }
        }
        
        ++node_count_;  // 分裂节点算新增1个
        return;
    }

    // === 情况3: 没有任何子边匹配 → 添加全新叶子 ===
    auto leaf = std::make_unique<Node>();
    leaf->edge = std::string(remaining);
    leaf->is_terminal = at_end_is_terminal;
    if (at_end_is_terminal && mark_as_prefix)
        leaf->is_prefix_marked = true;

    parent->children.push_back(std::move(leaf));
    ++node_count_;
}


// ============================================================
// do_find_prefix - 内部前缀查找（零拷贝 string_view 遍历）
//
// 返回值语义：
//   .match_length > 0     → 匹配到的长度（可能 < word.size() 表示部分匹配）
//   .match_length == 0    → 无任何匹配（甚至第一个字符就不匹配）
//   .is_registered        → 匹配终点是否是被显式标记为 prefix 的位置
// ============================================================

CompressTrie::MatchResult CompressTrie::do_find_prefix(const Node *node, std::string_view remaining)
{
    if (!node || remaining.empty())
    {
        return {0, false};
    }

    int total_matched = 0;

    while (!remaining.empty())
    {
        const Node *next = nullptr;

        // 在子节点中查找第一个字符匹配的边
        // 使用线性搜索（对于小规模子节点足够快；大规模可改为有序二分查找）
        for (const auto &child : node->children)
        {
            if (!child || child->edge.empty()) continue;
            
            if (child->edge[0] == remaining[0])
            {
                next = &*child;
                break;
            }
        }

        if (!next)
        {
            // 无法继续匹配
            break;
        }

        const std::string_view edge(next->edge);

        // 计算边与剩余输入的公共前缀长度
        size_t common = 0;
        size_t max_match = std::min(edge.size(), remaining.size());

#if defined(__GNUC__) || defined(__clang__)
        // GCC/Clang 有 __builtin_memcmp 可优化短字符串比较
        if (max_match >= 16)
#endif
        {
            while (common < max_match && edge[common] == remaining[common])
                ++common;
        }
#if defined(__GNUC__) || defined(__clang__)
        else
        {
            // 短字符串展开比较（减少函数调用开销）
            switch (max_match)
            {
            default:
                if (edge[common] != remaining[common]) goto MISMATCH;
                ++common; [[fallthrough]];
            case 15:
                if (edge[common] != remaining[common]) goto MISMATCH;
                ++common; [[fallthrough]];
            case 14:
                if (edge[common] != remaining[common]) goto MISMATCH;
                ++common; [[fallthrough]];
            case 13:
                if (edge[common] != remaining[common]) goto MISMATCH;
                ++common; [[fallthrough]];
            case 12:
                if (edge[common] != remaining[common]) goto MISMATCH;
                ++common; [[fallthrough]];
            case 11:
                if (edge[common] != remaining[common]) goto MISMATCH;
                ++common; [[fallthrough]];
            case 10:
                if (edge[common] != remaining[common]) goto MISMATCH;
                ++common; [[fallthrough]];
            case 9:
                if (edge[common] != remaining[common]) goto MISMATCH;
                ++common; [[fallthrough]];
            case 8:
                if (edge[common] != remaining[common]) goto MISMATCH;
                ++common; [[fallthrough]];
            case 7:
                if (edge[common] != remaining[common]) goto MISMATCH;
                ++common; [[fallthrough]];
            case 6:
                if (edge[common] != remaining[common]) goto MISMATCH;
                ++common; [[fallthrough]];
            case 5:
                if (edge[common] != remaining[common]) goto MISMATCH;
                ++common; [[fallthrough]];
            case 4:
                if (edge[common] != remaining[common]) goto MISMATCH;
                ++common; [[fallthrough]];
            case 3:
                if (edge[common] != remaining[common]) goto MISMATCH;
                ++common; [[fallthrough]];
            case 2:
                if (edge[common] != remaining[common]) goto MISMATCH;
                ++common; [[fallthrough]];
            case 1:
                if (edge[common] != remaining[common]) goto MISMATCH;
                ++common; [[fallthrough]];
            case 0:
                break;
            }
        MISMATCH:;
        }
#else
        // MSVC 或其他编译器：使用标准循环
        while (common < max_match && edge[common] == remaining[common])
            ++common;
#endif

        total_matched += static_cast<int>(common);

        if (common < edge.size())
        {
            // 边标签未完全消耗 → 在边的中间停止（部分匹配）
            // 这种情况不可能到达一个 terminal/prefix 节点
            return {total_matched, false};
        }

        // 整条边完全匹配
        if (common == remaining.size())
        {
            // 输入字符串恰好在这里结束
            return {total_matched, next->is_prefix_marked};
        }

        // 还有剩余字符，继续向下
        node = next;
        remaining = remaining.substr(common);
    }

    // 循环退出：可能是因为子节点没找到而break（部分前缀匹配），
    // 也可能是所有输入都消耗完毕。无论哪种情况，
    // 都需要检查最终停留节点的 is_prefix_marked 标志。
    return {total_matched, node && node->is_prefix_marked};
}

} // namespace yuan::base
