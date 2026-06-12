#include "game_base/algorithm/algorithms.h"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <vector>

namespace
{
    bool require(bool condition, const char *message)
    {
        if (!condition) {
            std::cerr << message << '\n';
            return false;
        }
        return true;
    }

    bool contains(const std::vector<yuan::game_base::EntityId> &values, yuan::game_base::EntityId id)
    {
        return std::find(values.begin(), values.end(), id) != values.end();
    }

    int test_astar()
    {
        using namespace yuan::game_base::algorithm;
        DenseGridMap map(8, 8);
        map.set_walkable({3, 0}, false);
        map.set_walkable({3, 1}, false);
        map.set_walkable({3, 2}, false);
        map.set_walkable({3, 3}, false);
        map.set_walkable({3, 4}, false);

        PathOptions options;
        options.allow_diagonal = false;
        const auto result = astar_search(map, {0, 0}, {7, 7}, options);
        if (!require(result.found, "astar should find path around wall")) {
            return 10;
        }
        if (!require(result.points.front() == GridPoint{0, 0} && result.points.back() == GridPoint{7, 7},
                     "astar path endpoints mismatch")) {
            return 11;
        }
        for (const auto point : result.points) {
            if (!require(map.walkable(point), "astar path should only use walkable cells")) {
                return 12;
            }
        }
        return 0;
    }

    int test_jps()
    {
        using namespace yuan::game_base::algorithm;
        DenseGridMap map(16, 16);
        const auto result = jps_search(map, {0, 0}, {15, 15});
        if (!require(result.found, "jps should find open-grid diagonal path")) {
            return 30;
        }
        if (!require(result.points.front() == GridPoint{0, 0} && result.points.back() == GridPoint{15, 15},
                     "jps path endpoints mismatch")) {
            return 31;
        }
        if (!require(result.points.size() <= 3, "jps should return jump-point compressed path on open grid")) {
            return 32;
        }
        return 0;
    }

    int test_aoi()
    {
        using namespace yuan::game_base::algorithm;
        GridAoi aoi(10.0F);
        if (!require(aoi.upsert({1, {0.0F, 0.0F}, 15.0F, {}}), "aoi insert observer should succeed")) {
            return 50;
        }
        if (!require(aoi.upsert({2, {5.0F, 0.0F}, 0.0F, {}}), "aoi insert near target should succeed")) {
            return 51;
        }
        if (!require(aoi.upsert({3, {30.0F, 0.0F}, 0.0F, {}}), "aoi insert far target should succeed")) {
            return 52;
        }

        const auto visible = aoi.visible_for(1);
        if (!require(contains(visible, 2) && !contains(visible, 3), "aoi visible set mismatch")) {
            return 53;
        }

        if (!require(aoi.upsert({2, {40.0F, 0.0F}, 0.0F, {}}), "aoi move target should succeed")) {
            return 54;
        }
        if (!require(aoi.upsert({3, {8.0F, 0.0F}, 0.0F, {}}), "aoi move far target near should succeed")) {
            return 55;
        }

        const auto diff = aoi.diff(1, visible);
        if (!require(contains(diff.entered, 3), "aoi diff should include entered entity")) {
            return 56;
        }
        if (!require(contains(diff.left, 2), "aoi diff should include left entity")) {
            return 57;
        }
        if (!require(aoi.remove(3) && aoi.size() == 2, "aoi remove should update size")) {
            return 58;
        }
        return 0;
    }

    int test_nine_grid_aoi()
    {
        using namespace yuan::game_base::algorithm;
        NineGridAoi aoi(10.0F);
        aoi.upsert({1, {5.0F, 5.0F}, 0.0F, {}});
        aoi.upsert({2, {15.0F, 5.0F}, 0.0F, {}});
        aoi.upsert({3, {35.0F, 5.0F}, 0.0F, {}});
        const auto visible = aoi.visible_for(1);
        if (!require(contains(visible, 2) && !contains(visible, 3), "nine-grid aoi visible set mismatch")) {
            return 70;
        }
        return 0;
    }

    int test_cross_list_aoi()
    {
        using namespace yuan::game_base::algorithm;
        CrossListAoi aoi;
        aoi.upsert({1, {0.0F, 0.0F}, 20.0F, {}});
        aoi.upsert({2, {10.0F, 0.0F}, 0.0F, {}});
        aoi.upsert({3, {10.0F, 30.0F}, 0.0F, {}});
        const auto visible = aoi.visible_for(1);
        if (!require(contains(visible, 2) && !contains(visible, 3), "cross-list aoi visible set mismatch")) {
            return 80;
        }
        return 0;
    }

    int test_lighthouse_aoi()
    {
        using namespace yuan::game_base::algorithm;
        LighthouseAoi aoi;
        if (!require(aoi.add_lighthouse({100, {0.0F, 0.0F}, 50.0F, {}}), "lighthouse add should succeed")) {
            return 90;
        }
        if (!require(aoi.add_lighthouse({101, {100.0F, 0.0F}, 50.0F, {}}), "second lighthouse add should succeed")) {
            return 91;
        }
        aoi.upsert({1, {0.0F, 0.0F}, 20.0F, {}});
        aoi.upsert({2, {10.0F, 0.0F}, 0.0F, {}});
        aoi.upsert({3, {100.0F, 0.0F}, 0.0F, {}});
        const auto visible = aoi.visible_for(1);
        if (!require(contains(visible, 2) && !contains(visible, 3), "lighthouse aoi visible set mismatch")) {
            return 92;
        }
        const auto assigned = aoi.lighthouses_for(1);
        if (!require(contains(assigned, 100), "lighthouse assignment mismatch")) {
            return 93;
        }
        return 0;
    }

    int test_path_smoothing_and_flow_field()
    {
        using namespace yuan::game_base::algorithm;
        DenseGridMap map(10, 10);
        PathOptions options;
        const auto path = astar_search(map, {0, 0}, {9, 9}, options);
        if (!require(path.found, "astar should find smoothing fixture path")) {
            return 100;
        }
        const auto smooth = smooth_path(map, path.points, options);
        if (!require(smooth.size() <= path.points.size() && smooth.front() == GridPoint{0, 0} && smooth.back() == GridPoint{9, 9},
                     "smooth path should preserve endpoints and reduce points")) {
            return 101;
        }

        FlowField field(map, {9, 9}, options);
        const auto traced = field.trace_path({0, 0});
        if (!require(!traced.empty() && traced.back() == GridPoint{9, 9}, "flow field should trace to target")) {
            return 102;
        }
        return 0;
    }

    int test_aoi_facade()
    {
        using namespace yuan::game_base::algorithm;
        AoiSystem aoi(AoiAlgorithmType::nine_grid, 10.0F);
        aoi.upsert({1, {0.0F, 0.0F}, 0.0F, {}});
        aoi.upsert({2, {9.0F, 0.0F}, 0.0F, {}});
        aoi.upsert({3, {30.0F, 0.0F}, 0.0F, {}});
        const auto visible = aoi.visible_for(1);
        if (!require(contains(visible, 2) && !contains(visible, 3), "aoi facade should delegate selected algorithm")) {
            return 110;
        }

        AoiSystem lighthouse(AoiAlgorithmType::lighthouse);
        if (!require(lighthouse.as_lighthouse().has_value(), "aoi facade should expose lighthouse config")) {
            return 111;
        }
        return 0;
    }
}

int main()
{
    if (const int rc = test_astar(); rc != 0) {
        return rc;
    }
    if (const int rc = test_jps(); rc != 0) {
        return rc;
    }
    if (const int rc = test_aoi(); rc != 0) {
        return rc;
    }
    if (const int rc = test_nine_grid_aoi(); rc != 0) {
        return rc;
    }
    if (const int rc = test_cross_list_aoi(); rc != 0) {
        return rc;
    }
    if (const int rc = test_lighthouse_aoi(); rc != 0) {
        return rc;
    }
    if (const int rc = test_path_smoothing_and_flow_field(); rc != 0) {
        return rc;
    }
    if (const int rc = test_aoi_facade(); rc != 0) {
        return rc;
    }
    return EXIT_SUCCESS;
}
