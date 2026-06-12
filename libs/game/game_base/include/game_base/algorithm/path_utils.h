#ifndef YUAN_GAME_BASE_ALGORITHM_PATH_UTILS_H
#define YUAN_GAME_BASE_ALGORITHM_PATH_UTILS_H

#include "game_base/algorithm/grid_pathfinding.h"

#include <cstdlib>
#include <vector>

namespace yuan::game_base::algorithm
{
    inline bool has_line_of_sight(const IGridMap &map, GridPoint from, GridPoint to, const PathOptions &options = {})
    {
        int x0 = from.x;
        int y0 = from.y;
        const int x1 = to.x;
        const int y1 = to.y;
        const int dx = std::abs(x1 - x0);
        const int dy = std::abs(y1 - y0);
        const int sx = x0 < x1 ? 1 : -1;
        const int sy = y0 < y1 ? 1 : -1;
        int err = dx - dy;

        GridPoint previous{x0, y0};
        while (true) {
            GridPoint current{x0, y0};
            if (!can_step(map, previous, current, options)) {
                return false;
            }
            if (x0 == x1 && y0 == y1) {
                return true;
            }
            previous = current;
            const int e2 = 2 * err;
            if (e2 > -dy) {
                err -= dy;
                x0 += sx;
            }
            if (e2 < dx) {
                err += dx;
                y0 += sy;
            }
        }
    }

    inline std::vector<GridPoint> smooth_path(const IGridMap &map, const std::vector<GridPoint> &path, const PathOptions &options = {})
    {
        if (path.size() <= 2) {
            return path;
        }

        std::vector<GridPoint> out;
        out.push_back(path.front());
        std::size_t anchor = 0;
        while (anchor < path.size() - 1) {
            std::size_t next = path.size() - 1;
            while (next > anchor + 1 && !has_line_of_sight(map, path[anchor], path[next], options)) {
                --next;
            }
            out.push_back(path[next]);
            anchor = next;
        }
        return out;
    }
}

#endif
