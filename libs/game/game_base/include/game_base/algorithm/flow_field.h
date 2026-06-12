#ifndef YUAN_GAME_BASE_ALGORITHM_FLOW_FIELD_H
#define YUAN_GAME_BASE_ALGORITHM_FLOW_FIELD_H

#include "game_base/algorithm/grid_pathfinding.h"

#include <algorithm>
#include <limits>
#include <queue>
#include <unordered_map>
#include <vector>

namespace yuan::game_base::algorithm
{
    struct FlowFieldCell
    {
        float cost = std::numeric_limits<float>::infinity();
        GridPoint direction{0, 0};
        bool reachable = false;
    };

    class FlowField
    {
    public:
        FlowField() = default;

        FlowField(const IGridMap &map, GridPoint target, PathOptions options = {})
        {
            build(map, target, options);
        }

        void build(const IGridMap &map, GridPoint target, PathOptions options = {})
        {
            target_ = target;
            cells_.clear();
            if (!map.walkable(target)) {
                return;
            }

            struct QueueItem
            {
                GridPoint point;
                float cost = 0.0F;
                bool operator<(const QueueItem &other) const
                {
                    return cost > other.cost;
                }
            };

            std::priority_queue<QueueItem> open;
            cells_[target] = {0.0F, {0, 0}, true};
            open.push({target, 0.0F});

            while (!open.empty()) {
                const auto current = open.top();
                open.pop();
                const auto current_cell = cells_.find(current.point);
                if (current_cell == cells_.end() || current.cost > current_cell->second.cost) {
                    continue;
                }

                for (const auto &next : neighbors(map, current.point, options)) {
                    const float step = (next.x != current.point.x && next.y != current.point.y) ? 1.41421356237F : 1.0F;
                    const float new_cost = current.cost + step * map.cost(next, current.point);
                    auto &cell = cells_[next];
                    if (!cell.reachable || new_cost < cell.cost) {
                        cell.cost = new_cost;
                        cell.reachable = true;
                        cell.direction = {sign(current.point.x - next.x), sign(current.point.y - next.y)};
                        open.push({next, new_cost});
                    }
                }
            }
        }

        [[nodiscard]] std::optional<FlowFieldCell> cell(GridPoint point) const
        {
            const auto it = cells_.find(point);
            if (it == cells_.end()) {
                return std::nullopt;
            }
            return it->second;
        }

        [[nodiscard]] GridPoint next_step(GridPoint point) const
        {
            const auto it = cells_.find(point);
            if (it == cells_.end() || !it->second.reachable) {
                return point;
            }
            return {point.x + it->second.direction.x, point.y + it->second.direction.y};
        }

        [[nodiscard]] std::vector<GridPoint> trace_path(GridPoint start, std::size_t max_steps = 1024) const
        {
            std::vector<GridPoint> path;
            GridPoint current = start;
            path.push_back(current);
            for (std::size_t i = 0; i < max_steps && current != target_; ++i) {
                const GridPoint next = next_step(current);
                if (next == current) {
                    break;
                }
                current = next;
                path.push_back(current);
            }
            return path;
        }

        [[nodiscard]] GridPoint target() const
        {
            return target_;
        }

        [[nodiscard]] std::size_t size() const
        {
            return cells_.size();
        }

    private:
        GridPoint target_{};
        std::unordered_map<GridPoint, FlowFieldCell, GridPointHash> cells_;
    };
}

#endif
