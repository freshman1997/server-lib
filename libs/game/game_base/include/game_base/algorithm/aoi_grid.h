#ifndef YUAN_GAME_BASE_ALGORITHM_AOI_GRID_H
#define YUAN_GAME_BASE_ALGORITHM_AOI_GRID_H

#include "game_base/types.h"

#include <algorithm>
#include <cmath>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace yuan::game_base::algorithm
{
    struct AoiPosition
    {
        float x = 0.0F;
        float y = 0.0F;
    };

    struct AoiCell
    {
        int x = 0;
        int y = 0;

        bool operator==(const AoiCell &other) const
        {
            return x == other.x && y == other.y;
        }
    };

    struct AoiCellHash
    {
        std::size_t operator()(const AoiCell &cell) const
        {
            std::size_t h = std::hash<int>{}(cell.x);
            h ^= std::hash<int>{}(cell.y) + 0x9e3779b9U + (h << 6U) + (h >> 2U);
            return h;
        }
    };

    struct AoiObject
    {
        EntityId id = 0;
        AoiPosition position;
        float view_radius = 0.0F;
        Tags tags;
    };

    struct AoiDiff
    {
        std::vector<EntityId> entered;
        std::vector<EntityId> left;
        std::vector<EntityId> stayed;
    };

    inline AoiDiff build_aoi_diff(const std::vector<EntityId> &previous_visible,
                                  const std::vector<EntityId> &current_visible)
    {
        AoiDiff result;
        std::unordered_set<EntityId> previous_set(previous_visible.begin(), previous_visible.end());
        std::unordered_set<EntityId> current_set(current_visible.begin(), current_visible.end());
        for (const auto id : current_visible) {
            if (previous_set.find(id) == previous_set.end()) {
                result.entered.push_back(id);
            } else {
                result.stayed.push_back(id);
            }
        }
        for (const auto id : previous_visible) {
            if (current_set.find(id) == current_set.end()) {
                result.left.push_back(id);
            }
        }
        return result;
    }

    class GridAoi
    {
    public:
        explicit GridAoi(float cell_size = 16.0F)
            : cell_size_(cell_size <= 0.0F ? 16.0F : cell_size)
        {
        }

        bool upsert(AoiObject object)
        {
            if (object.id == 0) {
                return false;
            }

            std::lock_guard<std::mutex> lock(mutex_);
            remove_from_cell_locked(object.id);
            const auto cell = cell_for(object.position);
            object_cells_[object.id] = cell;
            cells_[cell].insert(object.id);
            objects_[object.id] = std::move(object);
            return true;
        }

        bool remove(EntityId id)
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const bool removed_cell = remove_from_cell_locked(id);
            const bool removed_object = objects_.erase(id) != 0;
            return removed_cell || removed_object;
        }

        std::optional<AoiObject> find(EntityId id) const
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto it = objects_.find(id);
            if (it == objects_.end()) {
                return std::nullopt;
            }
            return it->second;
        }

        std::vector<EntityId> query(AoiPosition center, float radius, bool include_center_object = true) const
        {
            std::vector<EntityId> out;
            const float radius_sq = radius * radius;
            const int cells = static_cast<int>(std::ceil(radius / cell_size_));
            const auto center_cell = cell_for(center);

            std::lock_guard<std::mutex> lock(mutex_);
            for (int dy = -cells; dy <= cells; ++dy) {
                for (int dx = -cells; dx <= cells; ++dx) {
                    const auto it = cells_.find({center_cell.x + dx, center_cell.y + dy});
                    if (it == cells_.end()) {
                        continue;
                    }
                    for (const auto id : it->second) {
                        const auto object = objects_.find(id);
                        if (object == objects_.end()) {
                            continue;
                        }
                        const float ox = object->second.position.x - center.x;
                        const float oy = object->second.position.y - center.y;
                        if ((include_center_object || ox != 0.0F || oy != 0.0F) && ox * ox + oy * oy <= radius_sq) {
                            out.push_back(id);
                        }
                    }
                }
            }
            return out;
        }

        std::vector<EntityId> query_cells(AoiPosition center, int cell_radius, EntityId exclude = 0) const
        {
            std::vector<EntityId> out;
            const auto center_cell = cell_for(center);
            std::lock_guard<std::mutex> lock(mutex_);
            for (int dy = -cell_radius; dy <= cell_radius; ++dy) {
                for (int dx = -cell_radius; dx <= cell_radius; ++dx) {
                    const auto it = cells_.find({center_cell.x + dx, center_cell.y + dy});
                    if (it == cells_.end()) {
                        continue;
                    }
                    for (const auto id : it->second) {
                        if (id != exclude) {
                            out.push_back(id);
                        }
                    }
                }
            }
            return out;
        }

        std::vector<EntityId> visible_for(EntityId observer) const
        {
            const auto object = find(observer);
            if (!object.has_value()) {
                return {};
            }
            auto out = query(object->position, object->view_radius, false);
            out.erase(std::remove(out.begin(), out.end(), observer), out.end());
            return out;
        }

        AoiDiff diff(EntityId observer, const std::vector<EntityId> &previous_visible) const
        {
            AoiDiff result;
            return diff_from_current(observer, previous_visible, visible_for(observer));
        }

        AoiDiff diff_from_current(EntityId,
                                  const std::vector<EntityId> &previous_visible,
                                  const std::vector<EntityId> &current_visible) const
        {
            return build_aoi_diff(previous_visible, current_visible);
        }

        AoiCell cell_for(AoiPosition position) const
        {
            return {static_cast<int>(std::floor(position.x / cell_size_)),
                    static_cast<int>(std::floor(position.y / cell_size_))};
        }

        [[nodiscard]] std::size_t size() const
        {
            std::lock_guard<std::mutex> lock(mutex_);
            return objects_.size();
        }

    private:
        bool remove_from_cell_locked(EntityId id)
        {
            const auto cell_it = object_cells_.find(id);
            if (cell_it == object_cells_.end()) {
                return false;
            }
            const auto bucket_it = cells_.find(cell_it->second);
            if (bucket_it != cells_.end()) {
                bucket_it->second.erase(id);
                if (bucket_it->second.empty()) {
                    cells_.erase(bucket_it);
                }
            }
            object_cells_.erase(cell_it);
            return true;
        }

        float cell_size_ = 16.0F;
        mutable std::mutex mutex_;
        std::unordered_map<EntityId, AoiObject> objects_;
        std::unordered_map<EntityId, AoiCell> object_cells_;
        std::unordered_map<AoiCell, std::unordered_set<EntityId>, AoiCellHash> cells_;
    };
}

#endif
