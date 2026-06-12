#ifndef YUAN_GAME_BASE_ALGORITHM_GRID_PATHFINDING_H
#define YUAN_GAME_BASE_ALGORITHM_GRID_PATHFINDING_H

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <queue>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace yuan::game_base::algorithm
{
    struct GridPoint
    {
        int x = 0;
        int y = 0;

        bool operator==(const GridPoint &other) const
        {
            return x == other.x && y == other.y;
        }

        bool operator!=(const GridPoint &other) const
        {
            return !(*this == other);
        }
    };

    struct GridPointHash
    {
        std::size_t operator()(const GridPoint &point) const
        {
            std::size_t h = std::hash<int>{}(point.x);
            h ^= std::hash<int>{}(point.y) + 0x9e3779b9U + (h << 6U) + (h >> 2U);
            return h;
        }
    };

    class IGridMap
    {
    public:
        virtual ~IGridMap() = default;

        virtual bool in_bounds(GridPoint point) const = 0;
        virtual bool walkable(GridPoint point) const = 0;
        virtual float cost(GridPoint from, GridPoint to) const
        {
            (void)from;
            (void)to;
            return 1.0F;
        }
    };

    class DenseGridMap final : public IGridMap
    {
    public:
        DenseGridMap(int width, int height, bool default_walkable = true)
            : width_(std::max(0, width)),
              height_(std::max(0, height)),
              walkable_(static_cast<std::size_t>(width_) * static_cast<std::size_t>(height_), default_walkable)
        {
        }

        bool in_bounds(GridPoint point) const override
        {
            return point.x >= 0 && point.y >= 0 && point.x < width_ && point.y < height_;
        }

        bool walkable(GridPoint point) const override
        {
            return in_bounds(point) && walkable_[index(point)];
        }

        void set_walkable(GridPoint point, bool walkable)
        {
            if (in_bounds(point)) {
                walkable_[index(point)] = walkable;
            }
        }

        [[nodiscard]] int width() const
        {
            return width_;
        }

        [[nodiscard]] int height() const
        {
            return height_;
        }

    private:
        std::size_t index(GridPoint point) const
        {
            return static_cast<std::size_t>(point.y) * static_cast<std::size_t>(width_) + static_cast<std::size_t>(point.x);
        }

        int width_ = 0;
        int height_ = 0;
        std::vector<bool> walkable_;
    };

    struct PathOptions
    {
        bool allow_diagonal = true;
        bool prevent_corner_cutting = true;
        std::size_t max_visited = 100000;
    };

    struct PathResult
    {
        bool found = false;
        std::vector<GridPoint> points;
        std::size_t visited = 0;
        float cost = 0.0F;
    };

    inline int sign(int value)
    {
        return (value > 0) - (value < 0);
    }

    inline float octile_distance(GridPoint a, GridPoint b)
    {
        const auto dx = static_cast<float>(std::abs(a.x - b.x));
        const auto dy = static_cast<float>(std::abs(a.y - b.y));
        constexpr float diagonal = 1.41421356237F;
        return (dx + dy) + (diagonal - 2.0F) * std::min(dx, dy);
    }

    inline float manhattan_distance(GridPoint a, GridPoint b)
    {
        return static_cast<float>(std::abs(a.x - b.x) + std::abs(a.y - b.y));
    }

    inline bool can_step(const IGridMap &map, GridPoint from, GridPoint to, const PathOptions &options)
    {
        if (!map.walkable(to)) {
            return false;
        }
        const int dx = to.x - from.x;
        const int dy = to.y - from.y;
        if (dx != 0 && dy != 0 && options.prevent_corner_cutting) {
            return map.walkable({from.x + dx, from.y}) && map.walkable({from.x, from.y + dy});
        }
        return true;
    }

    inline std::vector<GridPoint> neighbors(const IGridMap &map, GridPoint point, const PathOptions &options)
    {
        static constexpr GridPoint cardinal[] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
        static constexpr GridPoint diagonal[] = {{1, 1}, {1, -1}, {-1, 1}, {-1, -1}};

        std::vector<GridPoint> out;
        out.reserve(options.allow_diagonal ? 8 : 4);
        for (const auto &dir : cardinal) {
            GridPoint next{point.x + dir.x, point.y + dir.y};
            if (can_step(map, point, next, options)) {
                out.push_back(next);
            }
        }
        if (options.allow_diagonal) {
            for (const auto &dir : diagonal) {
                GridPoint next{point.x + dir.x, point.y + dir.y};
                if (can_step(map, point, next, options)) {
                    out.push_back(next);
                }
            }
        }
        return out;
    }

    inline std::vector<GridPoint> reconstruct_path(const std::unordered_map<GridPoint, GridPoint, GridPointHash> &came_from,
                                                   GridPoint start,
                                                   GridPoint goal)
    {
        std::vector<GridPoint> path;
        GridPoint current = goal;
        path.push_back(current);
        while (current != start) {
            const auto it = came_from.find(current);
            if (it == came_from.end()) {
                return {};
            }
            current = it->second;
            path.push_back(current);
        }
        std::reverse(path.begin(), path.end());
        return path;
    }

    inline PathResult astar_search(const IGridMap &map, GridPoint start, GridPoint goal, PathOptions options = {})
    {
        PathResult result;
        if (!map.walkable(start) || !map.walkable(goal)) {
            return result;
        }

        struct QueueItem
        {
            GridPoint point;
            float priority = 0.0F;
            bool operator<(const QueueItem &other) const
            {
                return priority > other.priority;
            }
        };

        std::priority_queue<QueueItem> open;
        std::unordered_map<GridPoint, GridPoint, GridPointHash> came_from;
        std::unordered_map<GridPoint, float, GridPointHash> cost_so_far;
        open.push({start, 0.0F});
        cost_so_far[start] = 0.0F;

        while (!open.empty()) {
            const GridPoint current = open.top().point;
            open.pop();
            result.visited++;
            if (current == goal) {
                result.found = true;
                result.points = reconstruct_path(came_from, start, goal);
                result.cost = cost_so_far[current];
                return result;
            }
            if (result.visited >= options.max_visited) {
                return result;
            }

            for (const auto &next : neighbors(map, current, options)) {
                const float step = (next.x != current.x && next.y != current.y) ? 1.41421356237F : 1.0F;
                const float new_cost = cost_so_far[current] + step * map.cost(current, next);
                const auto it = cost_so_far.find(next);
                if (it == cost_so_far.end() || new_cost < it->second) {
                    cost_so_far[next] = new_cost;
                    const float heuristic = options.allow_diagonal ? octile_distance(next, goal) : manhattan_distance(next, goal);
                    open.push({next, new_cost + heuristic});
                    came_from[next] = current;
                }
            }
        }
        return result;
    }

    inline bool has_forced_neighbor(const IGridMap &map, GridPoint point, int dx, int dy, const PathOptions &options)
    {
        if (dx != 0 && dy != 0) {
            return (!map.walkable({point.x - dx, point.y}) && can_step(map, point, {point.x - dx, point.y + dy}, options)) ||
                   (!map.walkable({point.x, point.y - dy}) && can_step(map, point, {point.x + dx, point.y - dy}, options));
        }
        if (dx != 0) {
            return (!map.walkable({point.x, point.y + 1}) && can_step(map, point, {point.x + dx, point.y + 1}, options)) ||
                   (!map.walkable({point.x, point.y - 1}) && can_step(map, point, {point.x + dx, point.y - 1}, options));
        }
        return (!map.walkable({point.x + 1, point.y}) && can_step(map, point, {point.x + 1, point.y + dy}, options)) ||
               (!map.walkable({point.x - 1, point.y}) && can_step(map, point, {point.x - 1, point.y + dy}, options));
    }

    inline std::optional<GridPoint> jump(const IGridMap &map, GridPoint current, int dx, int dy, GridPoint goal, const PathOptions &options)
    {
        GridPoint next{current.x + dx, current.y + dy};
        if (!can_step(map, current, next, options)) {
            return std::nullopt;
        }
        if (next == goal || has_forced_neighbor(map, next, dx, dy, options)) {
            return next;
        }
        if (dx != 0 && dy != 0) {
            if (jump(map, next, dx, 0, goal, options).has_value() || jump(map, next, 0, dy, goal, options).has_value()) {
                return next;
            }
        }
        return jump(map, next, dx, dy, goal, options);
    }

    inline PathResult jps_search(const IGridMap &map, GridPoint start, GridPoint goal, PathOptions options = {})
    {
        options.allow_diagonal = true;
        PathResult result;
        if (!map.walkable(start) || !map.walkable(goal)) {
            return result;
        }

        struct QueueItem
        {
            GridPoint point;
            float priority = 0.0F;
            bool operator<(const QueueItem &other) const
            {
                return priority > other.priority;
            }
        };

        std::priority_queue<QueueItem> open;
        std::unordered_map<GridPoint, GridPoint, GridPointHash> came_from;
        std::unordered_map<GridPoint, float, GridPointHash> cost_so_far;
        open.push({start, 0.0F});
        cost_so_far[start] = 0.0F;

        while (!open.empty()) {
            const GridPoint current = open.top().point;
            open.pop();
            result.visited++;
            if (current == goal) {
                result.found = true;
                result.points = reconstruct_path(came_from, start, goal);
                result.cost = cost_so_far[current];
                return result;
            }
            if (result.visited >= options.max_visited) {
                return result;
            }

            for (const auto &next : neighbors(map, current, options)) {
                const int dx = sign(next.x - current.x);
                const int dy = sign(next.y - current.y);
                const auto jumped = jump(map, current, dx, dy, goal, options);
                if (!jumped.has_value()) {
                    continue;
                }
                const float new_cost = cost_so_far[current] + octile_distance(current, *jumped);
                const auto it = cost_so_far.find(*jumped);
                if (it == cost_so_far.end() || new_cost < it->second) {
                    cost_so_far[*jumped] = new_cost;
                    open.push({*jumped, new_cost + octile_distance(*jumped, goal)});
                    came_from[*jumped] = current;
                }
            }
        }
        return result;
    }
}

#endif
