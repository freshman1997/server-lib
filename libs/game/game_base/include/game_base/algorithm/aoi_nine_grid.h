#ifndef YUAN_GAME_BASE_ALGORITHM_AOI_NINE_GRID_H
#define YUAN_GAME_BASE_ALGORITHM_AOI_NINE_GRID_H

#include "game_base/algorithm/aoi_grid.h"

namespace yuan::game_base::algorithm
{
    class NineGridAoi
    {
    public:
        explicit NineGridAoi(float cell_size = 16.0F)
            : grid_(cell_size)
        {
        }

        bool upsert(AoiObject object)
        {
            return grid_.upsert(std::move(object));
        }

        bool remove(EntityId id)
        {
            return grid_.remove(id);
        }

        std::optional<AoiObject> find(EntityId id) const
        {
            return grid_.find(id);
        }

        std::vector<EntityId> visible_for(EntityId observer) const
        {
            const auto object = grid_.find(observer);
            if (!object.has_value()) {
                return {};
            }
            return grid_.query_cells(object->position, 1, observer);
        }

        AoiDiff diff(EntityId observer, const std::vector<EntityId> &previous_visible) const
        {
            return grid_.diff_from_current(observer, previous_visible, visible_for(observer));
        }

        [[nodiscard]] std::size_t size() const
        {
            return grid_.size();
        }

    private:
        GridAoi grid_;
    };
}

#endif
