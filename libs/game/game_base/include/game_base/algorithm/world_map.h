#ifndef YUAN_GAME_BASE_ALGORITHM_WORLD_MAP_H
#define YUAN_GAME_BASE_ALGORITHM_WORLD_MAP_H

#include "game_base/types.h"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <vector>

namespace yuan::game_base::algorithm
{
    struct TileCoord
    {
        int x = 0;
        int y = 0;

        bool operator==(const TileCoord &other) const
        {
            return x == other.x && y == other.y;
        }
    };

    struct TileCoordHash
    {
        std::size_t operator()(TileCoord coord) const
        {
            std::size_t h = std::hash<int>{}(coord.x);
            h ^= std::hash<int>{}(coord.y) + 0x9e3779b9U + (h << 6U) + (h >> 2U);
            return h;
        }
    };

    enum class TerrainType : std::uint8_t
    {
        plain,
        forest,
        hill,
        mountain,
        water,
        road,
        city,
        blocked
    };

    struct MapTile
    {
        TerrainType terrain = TerrainType::plain;
        std::uint16_t level = 0;
        std::uint32_t resource = 0;
        EntityId owner = 0;
        bool passable = true;
        Tags tags;
    };

    struct MapRect
    {
        TileCoord min;
        TileCoord max;
    };

    inline int tile_manhattan(TileCoord a, TileCoord b)
    {
        return std::abs(a.x - b.x) + std::abs(a.y - b.y);
    }

    inline int tile_chebyshev(TileCoord a, TileCoord b)
    {
        return std::max(std::abs(a.x - b.x), std::abs(a.y - b.y));
    }

    inline std::vector<TileCoord> tile_neighbors4(TileCoord coord)
    {
        return {{coord.x + 1, coord.y}, {coord.x - 1, coord.y}, {coord.x, coord.y + 1}, {coord.x, coord.y - 1}};
    }

    inline std::vector<TileCoord> tile_neighbors8(TileCoord coord)
    {
        return {{coord.x + 1, coord.y},     {coord.x - 1, coord.y},     {coord.x, coord.y + 1},     {coord.x, coord.y - 1},
                {coord.x + 1, coord.y + 1}, {coord.x + 1, coord.y - 1}, {coord.x - 1, coord.y + 1}, {coord.x - 1, coord.y - 1}};
    }

    inline std::vector<TileCoord> tiles_in_rect(MapRect rect)
    {
        std::vector<TileCoord> out;
        for (int y = rect.min.y; y <= rect.max.y; ++y) {
            for (int x = rect.min.x; x <= rect.max.x; ++x) {
                out.push_back({x, y});
            }
        }
        return out;
    }

    inline std::vector<TileCoord> tiles_in_diamond(TileCoord center, int radius)
    {
        std::vector<TileCoord> out;
        for (int dy = -radius; dy <= radius; ++dy) {
            const int width = radius - std::abs(dy);
            for (int dx = -width; dx <= width; ++dx) {
                out.push_back({center.x + dx, center.y + dy});
            }
        }
        return out;
    }

    inline std::vector<TileCoord> tiles_in_square(TileCoord center, int radius)
    {
        return tiles_in_rect({{center.x - radius, center.y - radius}, {center.x + radius, center.y + radius}});
    }

    inline std::vector<TileCoord> tiles_in_ring(TileCoord center, int radius)
    {
        std::vector<TileCoord> out;
        if (radius <= 0) {
            out.push_back(center);
            return out;
        }
        for (int dy = -radius; dy <= radius; ++dy) {
            for (int dx = -radius; dx <= radius; ++dx) {
                if (std::max(std::abs(dx), std::abs(dy)) == radius) {
                    out.push_back({center.x + dx, center.y + dy});
                }
            }
        }
        return out;
    }

    inline std::vector<TileCoord> tiles_in_spiral(TileCoord center, int radius)
    {
        std::vector<TileCoord> out;
        out.push_back(center);
        for (int r = 1; r <= radius; ++r) {
            auto ring = tiles_in_ring(center, r);
            out.insert(out.end(), ring.begin(), ring.end());
        }
        return out;
    }

    class ChunkedWorldMap
    {
    public:
        explicit ChunkedWorldMap(int chunk_size = 32)
            : chunk_size_(chunk_size <= 0 ? 32 : chunk_size)
        {
        }

        void set_tile(TileCoord coord, MapTile tile)
        {
            tiles_[coord] = std::move(tile);
        }

        std::optional<MapTile> tile(TileCoord coord) const
        {
            const auto it = tiles_.find(coord);
            if (it == tiles_.end()) {
                return std::nullopt;
            }
            return it->second;
        }

        MapTile tile_or_default(TileCoord coord) const
        {
            const auto it = tiles_.find(coord);
            if (it == tiles_.end()) {
                return {};
            }
            return it->second;
        }

        bool passable(TileCoord coord) const
        {
            const auto value = tile(coord);
            return !value.has_value() || value->passable;
        }

        std::vector<TileCoord> query_owned(EntityId owner, MapRect rect) const
        {
            std::vector<TileCoord> out;
            for (const auto coord : tiles_in_rect(rect)) {
                const auto value = tile(coord);
                if (value.has_value() && value->owner == owner) {
                    out.push_back(coord);
                }
            }
            return out;
        }

        TileCoord chunk_of(TileCoord coord) const
        {
            return {floor_div(coord.x, chunk_size_), floor_div(coord.y, chunk_size_)};
        }

        [[nodiscard]] int chunk_size() const
        {
            return chunk_size_;
        }

        [[nodiscard]] std::size_t size() const
        {
            return tiles_.size();
        }

    private:
        static int floor_div(int value, int divisor)
        {
            int q = value / divisor;
            int r = value % divisor;
            if (r != 0 && ((r < 0) != (divisor < 0))) {
                --q;
            }
            return q;
        }

        int chunk_size_ = 32;
        std::unordered_map<TileCoord, MapTile, TileCoordHash> tiles_;
    };
}

#endif
