#ifndef YUAN_GAME_BASE_ALGORITHM_AOI_CROSS_LIST_H
#define YUAN_GAME_BASE_ALGORITHM_AOI_CROSS_LIST_H

#include "game_base/algorithm/aoi_grid.h"

#include <algorithm>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace yuan::game_base::algorithm
{
    class CrossListAoi
    {
    public:
        bool upsert(AoiObject object)
        {
            if (object.id == 0) {
                return false;
            }
            std::lock_guard<std::mutex> lock(mutex_);
            objects_[object.id] = std::move(object);
            rebuild_axis_locked();
            return true;
        }

        bool remove(EntityId id)
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (objects_.erase(id) == 0) {
                return false;
            }
            rebuild_axis_locked();
            return true;
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

        std::vector<EntityId> visible_for(EntityId observer) const
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto observer_it = objects_.find(observer);
            if (observer_it == objects_.end()) {
                return {};
            }

            const auto &object = observer_it->second;
            const float min_x = object.position.x - object.view_radius;
            const float max_x = object.position.x + object.view_radius;
            const float min_y = object.position.y - object.view_radius;
            const float max_y = object.position.y + object.view_radius;

            std::unordered_set<EntityId> x_candidates;
            for (const auto &[x, id] : x_axis_) {
                if (x > max_x) {
                    break;
                }
                if (x >= min_x && id != observer) {
                    x_candidates.insert(id);
                }
            }

            std::vector<EntityId> out;
            const float radius_sq = object.view_radius * object.view_radius;
            for (const auto &[y, id] : y_axis_) {
                if (y > max_y) {
                    break;
                }
                if (y < min_y || id == observer || x_candidates.find(id) == x_candidates.end()) {
                    continue;
                }
                const auto target_it = objects_.find(id);
                if (target_it == objects_.end()) {
                    continue;
                }
                const float dx = target_it->second.position.x - object.position.x;
                const float dy = target_it->second.position.y - object.position.y;
                if (dx * dx + dy * dy <= radius_sq) {
                    out.push_back(id);
                }
            }
            return out;
        }

        AoiDiff diff(EntityId observer, const std::vector<EntityId> &previous_visible) const
        {
            const auto current = visible_for(observer);
            return build_aoi_diff(previous_visible, current);
        }

        [[nodiscard]] std::size_t size() const
        {
            std::lock_guard<std::mutex> lock(mutex_);
            return objects_.size();
        }

    private:
        void rebuild_axis_locked()
        {
            x_axis_.clear();
            y_axis_.clear();
            x_axis_.reserve(objects_.size());
            y_axis_.reserve(objects_.size());
            for (const auto &[id, object] : objects_) {
                x_axis_.emplace_back(object.position.x, id);
                y_axis_.emplace_back(object.position.y, id);
            }
            std::sort(x_axis_.begin(), x_axis_.end(), [](const auto &a, const auto &b) {
                return a.first < b.first;
            });
            std::sort(y_axis_.begin(), y_axis_.end(), [](const auto &a, const auto &b) {
                return a.first < b.first;
            });
        }

        mutable std::mutex mutex_;
        std::unordered_map<EntityId, AoiObject> objects_;
        std::vector<std::pair<float, EntityId>> x_axis_;
        std::vector<std::pair<float, EntityId>> y_axis_;
    };
}

#endif
