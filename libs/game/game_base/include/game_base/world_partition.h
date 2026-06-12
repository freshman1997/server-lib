#ifndef YUAN_GAME_BASE_WORLD_PARTITION_H
#define YUAN_GAME_BASE_WORLD_PARTITION_H

#include "game_base/types.h"

#include <cmath>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>

namespace yuan::game_base
{
    struct Vec3
    {
        float x = 0.0F;
        float y = 0.0F;
        float z = 0.0F;
    };

    struct EntityLocation
    {
        EntityId entity = 0;
        SceneId scene = 0;
        Vec3 position;
        NodeId owner_node = 0;
    };

    struct CellKey
    {
        SceneId scene = 0;
        int x = 0;
        int z = 0;

        bool operator==(const CellKey &other) const
        {
            return scene == other.scene && x == other.x && z == other.z;
        }
    };

    struct CellKeyHash
    {
        std::size_t operator()(const CellKey &key) const
        {
            std::size_t h = std::hash<SceneId>{}(key.scene);
            h ^= std::hash<int>{}(key.x) + 0x9e3779b9U + (h << 6U) + (h >> 2U);
            h ^= std::hash<int>{}(key.z) + 0x9e3779b9U + (h << 6U) + (h >> 2U);
            return h;
        }
    };

    class WorldPartition
    {
    public:
        explicit WorldPartition(float cell_size = 32.0F)
            : cell_size_(cell_size <= 0.0F ? 32.0F : cell_size)
        {
        }

        bool upsert(EntityLocation location)
        {
            if (location.entity == 0) {
                return false;
            }

            std::lock_guard<std::mutex> lock(mutex_);
            locations_[location.entity] = location;
            return true;
        }

        bool remove(EntityId entity)
        {
            std::lock_guard<std::mutex> lock(mutex_);
            return locations_.erase(entity) != 0;
        }

        std::optional<EntityLocation> find(EntityId entity) const
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto it = locations_.find(entity);
            if (it == locations_.end()) {
                return std::nullopt;
            }
            return it->second;
        }

        std::vector<EntityLocation> query_cell(CellKey cell) const
        {
            std::vector<EntityLocation> out;
            std::lock_guard<std::mutex> lock(mutex_);
            for (const auto &[id, location] : locations_) {
                (void)id;
                if (cell_for(location.scene, location.position) == cell) {
                    out.push_back(location);
                }
            }
            return out;
        }

        CellKey cell_for(SceneId scene, Vec3 position) const
        {
            return {scene,
                    static_cast<int>(std::floor(position.x / cell_size_)),
                    static_cast<int>(std::floor(position.z / cell_size_))};
        }

    private:
        float cell_size_ = 32.0F;
        mutable std::mutex mutex_;
        std::unordered_map<EntityId, EntityLocation> locations_;
    };
}

#endif
