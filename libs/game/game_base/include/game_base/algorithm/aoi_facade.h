#ifndef YUAN_GAME_BASE_ALGORITHM_AOI_FACADE_H
#define YUAN_GAME_BASE_ALGORITHM_AOI_FACADE_H

#include "game_base/algorithm/aoi_cross_list.h"
#include "game_base/algorithm/aoi_grid.h"
#include "game_base/algorithm/aoi_lighthouse.h"
#include "game_base/algorithm/aoi_nine_grid.h"

#include <memory>
#include <optional>
#include <functional>
#include <variant>

namespace yuan::game_base::algorithm
{
    enum class AoiAlgorithmType
    {
        grid,
        nine_grid,
        cross_list,
        lighthouse
    };

    class AoiSystem
    {
    public:
        explicit AoiSystem(AoiAlgorithmType type = AoiAlgorithmType::grid, float cell_size = 16.0F)
            : type_(type)
        {
            switch (type) {
                case AoiAlgorithmType::grid:
                    impl_.emplace<GridAoi>(cell_size);
                    break;
                case AoiAlgorithmType::nine_grid:
                    impl_.emplace<NineGridAoi>(cell_size);
                    break;
                case AoiAlgorithmType::cross_list:
                    impl_.emplace<CrossListAoi>();
                    break;
                case AoiAlgorithmType::lighthouse:
                    impl_.emplace<LighthouseAoi>();
                    break;
            }
        }

        bool upsert(AoiObject object)
        {
            return std::visit([&](auto &impl) { return impl.upsert(std::move(object)); }, impl_);
        }

        bool remove(EntityId id)
        {
            return std::visit([&](auto &impl) { return impl.remove(id); }, impl_);
        }

        std::vector<EntityId> visible_for(EntityId observer) const
        {
            return std::visit([&](const auto &impl) { return impl.visible_for(observer); }, impl_);
        }

        AoiDiff diff(EntityId observer, const std::vector<EntityId> &previous_visible) const
        {
            return std::visit([&](const auto &impl) { return impl.diff(observer, previous_visible); }, impl_);
        }

        std::optional<std::reference_wrapper<LighthouseAoi>> as_lighthouse()
        {
            auto *value = std::get_if<LighthouseAoi>(&impl_);
            if (!value) {
                return std::nullopt;
            }
            return *value;
        }

        [[nodiscard]] AoiAlgorithmType type() const
        {
            return type_;
        }

    private:
        AoiAlgorithmType type_ = AoiAlgorithmType::grid;
        std::variant<GridAoi, NineGridAoi, CrossListAoi, LighthouseAoi> impl_;
    };
}

#endif
