#ifndef YUAN_GAME_BASE_ALGORITHM_AOI_LIGHTHOUSE_H
#define YUAN_GAME_BASE_ALGORITHM_AOI_LIGHTHOUSE_H

#include "game_base/algorithm/aoi_grid.h"

#include <algorithm>
#include <cmath>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace yuan::game_base::algorithm
{
    using LighthouseId = std::uint64_t;

    struct Lighthouse
    {
        LighthouseId id = 0;
        AoiPosition position;
        float radius = 0.0F;
        Tags tags;
    };

    class LighthouseAoi
    {
    public:
        bool add_lighthouse(Lighthouse lighthouse)
        {
            if (lighthouse.id == 0 || lighthouse.radius <= 0.0F) {
                return false;
            }
            std::lock_guard<std::mutex> lock(mutex_);
            lighthouses_[lighthouse.id] = std::move(lighthouse);
            rebuild_assignments_locked();
            return true;
        }

        bool remove_lighthouse(LighthouseId id)
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (lighthouses_.erase(id) == 0) {
                return false;
            }
            lighthouse_objects_.erase(id);
            rebuild_assignments_locked();
            return true;
        }

        bool upsert(AoiObject object)
        {
            if (object.id == 0) {
                return false;
            }
            std::lock_guard<std::mutex> lock(mutex_);
            remove_object_assignments_locked(object.id);
            objects_[object.id] = std::move(object);
            assign_object_locked(objects_.at(object.id));
            return true;
        }

        bool remove(EntityId id)
        {
            std::lock_guard<std::mutex> lock(mutex_);
            remove_object_assignments_locked(id);
            return objects_.erase(id) != 0;
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

        std::vector<LighthouseId> lighthouses_for(EntityId id) const
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto it = object_lighthouses_.find(id);
            if (it == object_lighthouses_.end()) {
                return {};
            }
            return {it->second.begin(), it->second.end()};
        }

        std::vector<EntityId> visible_for(EntityId observer) const
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto observer_it = objects_.find(observer);
            if (observer_it == objects_.end()) {
                return {};
            }

            std::unordered_set<EntityId> candidates;
            const auto assignment_it = object_lighthouses_.find(observer);
            if (assignment_it != object_lighthouses_.end()) {
                for (const auto lighthouse_id : assignment_it->second) {
                    const auto bucket_it = lighthouse_objects_.find(lighthouse_id);
                    if (bucket_it != lighthouse_objects_.end()) {
                        candidates.insert(bucket_it->second.begin(), bucket_it->second.end());
                    }
                }
            }

            std::vector<EntityId> out;
            const auto &object = observer_it->second;
            const float radius_sq = object.view_radius * object.view_radius;
            for (const auto id : candidates) {
                if (id == observer) {
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
            return build_aoi_diff(previous_visible, visible_for(observer));
        }

        [[nodiscard]] std::size_t size() const
        {
            std::lock_guard<std::mutex> lock(mutex_);
            return objects_.size();
        }

    private:
        static bool in_lighthouse(const AoiObject &object, const Lighthouse &lighthouse)
        {
            const float dx = object.position.x - lighthouse.position.x;
            const float dy = object.position.y - lighthouse.position.y;
            return dx * dx + dy * dy <= lighthouse.radius * lighthouse.radius;
        }

        void assign_object_locked(const AoiObject &object)
        {
            auto &assigned = object_lighthouses_[object.id];
            for (const auto &[id, lighthouse] : lighthouses_) {
                if (in_lighthouse(object, lighthouse)) {
                    assigned.insert(id);
                    lighthouse_objects_[id].insert(object.id);
                }
            }
        }

        void remove_object_assignments_locked(EntityId id)
        {
            const auto it = object_lighthouses_.find(id);
            if (it == object_lighthouses_.end()) {
                return;
            }
            for (const auto lighthouse_id : it->second) {
                const auto bucket_it = lighthouse_objects_.find(lighthouse_id);
                if (bucket_it != lighthouse_objects_.end()) {
                    bucket_it->second.erase(id);
                }
            }
            object_lighthouses_.erase(it);
        }

        void rebuild_assignments_locked()
        {
            lighthouse_objects_.clear();
            object_lighthouses_.clear();
            for (const auto &[id, object] : objects_) {
                (void)id;
                assign_object_locked(object);
            }
        }

        mutable std::mutex mutex_;
        std::unordered_map<LighthouseId, Lighthouse> lighthouses_;
        std::unordered_map<EntityId, AoiObject> objects_;
        std::unordered_map<EntityId, std::unordered_set<LighthouseId>> object_lighthouses_;
        std::unordered_map<LighthouseId, std::unordered_set<EntityId>> lighthouse_objects_;
    };
}

#endif
