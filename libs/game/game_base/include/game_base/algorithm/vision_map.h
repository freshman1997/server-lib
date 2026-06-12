#ifndef YUAN_GAME_BASE_ALGORITHM_VISION_MAP_H
#define YUAN_GAME_BASE_ALGORITHM_VISION_MAP_H

#include "game_base/algorithm/world_map.h"

#include <algorithm>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace yuan::game_base::algorithm
{
    using VisionGroupId = std::uint64_t;

    enum class VisionState : std::uint8_t
    {
        unseen,
        explored,
        visible
    };

    struct VisionSource
    {
        EntityId id = 0;
        VisionGroupId group = 0;
        TileCoord center;
        int radius = 0;
        bool diamond = true;
    };

    class VisionMap
    {
    public:
        bool upsert_source(VisionSource source)
        {
            if (source.id == 0 || source.group == 0 || source.radius < 0) {
                return false;
            }
            sources_[source.id] = source;
            rebuild_group(source.group);
            return true;
        }

        bool remove_source(EntityId id)
        {
            const auto it = sources_.find(id);
            if (it == sources_.end()) {
                return false;
            }
            const auto group = it->second.group;
            sources_.erase(it);
            rebuild_group(group);
            return true;
        }

        VisionState state(VisionGroupId group, TileCoord coord) const
        {
            const auto explored_it = explored_.find(group);
            const auto visible_it = visible_.find(group);
            if (visible_it != visible_.end() && visible_it->second.find(coord) != visible_it->second.end()) {
                return VisionState::visible;
            }
            if (explored_it != explored_.end() && explored_it->second.find(coord) != explored_it->second.end()) {
                return VisionState::explored;
            }
            return VisionState::unseen;
        }

        std::vector<TileCoord> visible_tiles(VisionGroupId group) const
        {
            const auto it = visible_.find(group);
            if (it == visible_.end()) {
                return {};
            }
            return {it->second.begin(), it->second.end()};
        }

        std::vector<TileCoord> explored_tiles(VisionGroupId group) const
        {
            const auto it = explored_.find(group);
            if (it == explored_.end()) {
                return {};
            }
            return {it->second.begin(), it->second.end()};
        }

        void share_vision(VisionGroupId from, VisionGroupId to)
        {
            auto &target_visible = visible_[to];
            auto &target_explored = explored_[to];
            const auto visible_it = visible_.find(from);
            if (visible_it != visible_.end()) {
                target_visible.insert(visible_it->second.begin(), visible_it->second.end());
                target_explored.insert(visible_it->second.begin(), visible_it->second.end());
            }
            const auto explored_it = explored_.find(from);
            if (explored_it != explored_.end()) {
                target_explored.insert(explored_it->second.begin(), explored_it->second.end());
            }
        }

    private:
        void rebuild_group(VisionGroupId group)
        {
            auto &visible = visible_[group];
            auto &explored = explored_[group];
            visible.clear();
            for (const auto &[id, source] : sources_) {
                (void)id;
                if (source.group != group) {
                    continue;
                }
                const auto tiles = source.diamond ? tiles_in_diamond(source.center, source.radius) : tiles_in_square(source.center, source.radius);
                visible.insert(tiles.begin(), tiles.end());
                explored.insert(tiles.begin(), tiles.end());
            }
        }

        std::unordered_map<EntityId, VisionSource> sources_;
        std::unordered_map<VisionGroupId, std::unordered_set<TileCoord, TileCoordHash>> visible_;
        std::unordered_map<VisionGroupId, std::unordered_set<TileCoord, TileCoordHash>> explored_;
    };
}

#endif
