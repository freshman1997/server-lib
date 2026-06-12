#ifndef YUAN_GAME_BASE_ALGORITHM_INFLUENCE_MAP_H
#define YUAN_GAME_BASE_ALGORITHM_INFLUENCE_MAP_H

#include "game_base/algorithm/world_map.h"

#include <algorithm>
#include <unordered_map>
#include <vector>

namespace yuan::game_base::algorithm
{
    struct InfluenceSource
    {
        EntityId owner = 0;
        TileCoord center;
        float strength = 0.0F;
        int radius = 0;
    };

    struct TerritoryCell
    {
        EntityId owner = 0;
        float value = 0.0F;
    };

    class InfluenceMap
    {
    public:
        void clear()
        {
            values_.clear();
        }

        void add_source(const InfluenceSource &source)
        {
            if (source.owner == 0 || source.radius < 0 || source.strength <= 0.0F) {
                return;
            }
            for (const auto coord : tiles_in_diamond(source.center, source.radius)) {
                const int distance = tile_manhattan(source.center, coord);
                const float value = source.strength * (1.0F - static_cast<float>(distance) / static_cast<float>(source.radius + 1));
                if (value <= 0.0F) {
                    continue;
                }
                values_[coord][source.owner] += value;
            }
        }

        std::unordered_map<EntityId, float> values(TileCoord coord) const
        {
            const auto it = values_.find(coord);
            if (it == values_.end()) {
                return {};
            }
            return it->second;
        }

        TerritoryCell dominant(TileCoord coord) const
        {
            TerritoryCell out;
            const auto it = values_.find(coord);
            if (it == values_.end()) {
                return out;
            }
            for (const auto &[owner, value] : it->second) {
                if (value > out.value) {
                    out.owner = owner;
                    out.value = value;
                }
            }
            return out;
        }

        std::vector<TileCoord> border_tiles(EntityId owner) const
        {
            std::vector<TileCoord> out;
            for (const auto &[coord, values] : values_) {
                (void)values;
                if (dominant(coord).owner != owner) {
                    continue;
                }
                for (const auto neighbor : tile_neighbors4(coord)) {
                    if (dominant(neighbor).owner != owner) {
                        out.push_back(coord);
                        break;
                    }
                }
            }
            return out;
        }

        [[nodiscard]] std::size_t size() const
        {
            return values_.size();
        }

    private:
        std::unordered_map<TileCoord, std::unordered_map<EntityId, float>, TileCoordHash> values_;
    };
}

#endif
