#ifndef YUAN_BASE_ALGORITHM_CONSISTENT_HASH_H_
#define YUAN_BASE_ALGORITHM_CONSISTENT_HASH_H_

#include <cstddef>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <utility>

namespace yuan::base
{
    // ConsistentHash 是一致性哈希环，适合把 key 稳定映射到节点，节点增删时尽量
    // 减少迁移范围。
    //
    // 适用场景：分片路由、缓存节点选择、服务实例选择、房间/用户分区。
    // 用法：
    //   yuan::base::ConsistentHash<std::string> hash;
    //   hash.add_node("node-a", "node-a");
    //   auto node = hash.get_node("user-1001");
    template <typename Node, typename Hash = std::hash<std::string>>
    class ConsistentHash
    {
    public:
        explicit ConsistentHash(std::size_t virtual_nodes = 128, Hash hash = Hash{})
            : virtual_nodes_(virtual_nodes), hash_(std::move(hash))
        {
        }

        void add_node(const Node &node, const std::string &key)
        {
            for (std::size_t i = 0; i < virtual_nodes_; ++i) {
                ring_[hash_(key + "#" + std::to_string(i))] = node;
            }
        }

        void remove_node(const std::string &key)
        {
            for (std::size_t i = 0; i < virtual_nodes_; ++i) {
                ring_.erase(hash_(key + "#" + std::to_string(i)));
            }
        }

        std::optional<Node> get_node(const std::string &key) const
        {
            if (ring_.empty()) {
                return std::nullopt;
            }

            auto it = ring_.lower_bound(hash_(key));
            if (it == ring_.end()) {
                it = ring_.begin();
            }
            return it->second;
        }

        void clear()
        {
            ring_.clear();
        }

        std::size_t ring_size() const noexcept { return ring_.size(); }
        bool empty() const noexcept { return ring_.empty(); }

    private:
        std::size_t virtual_nodes_ = 128;
        Hash hash_;
        std::map<std::size_t, Node> ring_;
    };
}

#endif // YUAN_BASE_ALGORITHM_CONSISTENT_HASH_H_
