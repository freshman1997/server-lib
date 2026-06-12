#include "game_base/algorithm/algorithms.h"

#include <cstdlib>
#include <iostream>

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

    int test_world_map_tiles()
    {
        using namespace yuan::game_base::algorithm;
        ChunkedWorldMap map(16);
        MapTile city;
        city.terrain = TerrainType::city;
        city.owner = 100;
        map.set_tile({3, 4}, city);

        MapTile water;
        water.terrain = TerrainType::water;
        water.passable = false;
        map.set_tile({20, 4}, water);

        if (!require(map.tile({3, 4}).has_value() && map.tile({3, 4})->owner == 100, "world map tile lookup mismatch")) {
            return 10;
        }
        if (!require(!map.passable({20, 4}), "world map passable should respect blocked tile")) {
            return 11;
        }
        if (!require(map.chunk_of({31, 0}) == TileCoord{1, 0} && map.chunk_of({-1, 0}) == TileCoord{-1, 0},
                     "world map chunk coord mismatch")) {
            return 12;
        }
        const auto diamond = tiles_in_diamond({0, 0}, 2);
        if (!require(diamond.size() == 13, "diamond range size mismatch")) {
            return 13;
        }
        const auto spiral = tiles_in_spiral({0, 0}, 2);
        if (!require(spiral.front() == TileCoord{0, 0} && spiral.size() == 25, "spiral range mismatch")) {
            return 14;
        }
        return 0;
    }

    int test_influence_map()
    {
        using namespace yuan::game_base::algorithm;
        InfluenceMap influence;
        influence.add_source({1, {0, 0}, 10.0F, 3});
        influence.add_source({2, {4, 0}, 10.0F, 3});
        if (!require(influence.dominant({0, 0}).owner == 1, "influence owner at source mismatch")) {
            return 30;
        }
        if (!require(influence.dominant({4, 0}).owner == 2, "influence owner at second source mismatch")) {
            return 31;
        }
        const auto border = influence.border_tiles(1);
        if (!require(!border.empty(), "influence should produce border tiles")) {
            return 32;
        }
        return 0;
    }

    int test_vision_map()
    {
        using namespace yuan::game_base::algorithm;
        VisionMap vision;
        if (!require(vision.upsert_source({10, 1, {0, 0}, 2, true}), "vision source insert should succeed")) {
            return 50;
        }
        if (!require(vision.state(1, {0, 0}) == VisionState::visible, "vision center should be visible")) {
            return 51;
        }
        if (!require(vision.state(1, {3, 0}) == VisionState::unseen, "out of range tile should be unseen")) {
            return 52;
        }
        if (!require(vision.remove_source(10), "vision source remove should succeed")) {
            return 53;
        }
        if (!require(vision.state(1, {0, 0}) == VisionState::explored, "removed visible tile should remain explored")) {
            return 54;
        }
        vision.upsert_source({11, 2, {10, 0}, 1, false});
        vision.share_vision(2, 1);
        if (!require(vision.state(1, {10, 0}) == VisionState::visible, "shared vision should become visible")) {
            return 55;
        }
        return 0;
    }
}

int main()
{
    if (const int rc = test_world_map_tiles(); rc != 0) {
        return rc;
    }
    if (const int rc = test_influence_map(); rc != 0) {
        return rc;
    }
    if (const int rc = test_vision_map(); rc != 0) {
        return rc;
    }
    return EXIT_SUCCESS;
}
