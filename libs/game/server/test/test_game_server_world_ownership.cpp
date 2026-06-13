#include "world/model/world_ownership_store.h"
#include "world/rpc/world_msg.h"

#include <cstdlib>
#include <iostream>
#include <memory>

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
}

int main()
{
    using namespace yuan::game::server;

    const auto zone_a = pack_game_service_id(1, 1, GameServiceType::zone, 1);
    const auto zone_b = pack_game_service_id(1, 1, GameServiceType::zone, 2);
    const RoleId role_id = 10001;

    InMemoryWorldOwnershipStore store;
    if (!require(store.compare_and_set(role_id, 0, 0, WorldOwnershipRecord{zone_a, 10}), "initial owner should set")) {
        return 1;
    }
    if (!require(store.compare_and_set(role_id, zone_a, 10, WorldOwnershipRecord{zone_b, 20}), "newer login should update owner")) {
        return 2;
    }
    if (!require(!store.compare_and_set(role_id, zone_a, 10, WorldOwnershipRecord{0, 0}), "old zone logout should be rejected")) {
        return 3;
    }
    if (!require(store.get(role_id) && store.get(role_id)->zone_service_id == zone_b, "old zone logout must not clear owner")) {
        return 4;
    }
    if (!require(!store.compare_and_set(role_id, zone_b, 10, WorldOwnershipRecord{0, 0}), "old session logout should be rejected")) {
        return 5;
    }
    if (!require(store.compare_and_set(role_id, zone_b, 20, WorldOwnershipRecord{0, 0}), "current session logout should clear owner")) {
        return 6;
    }
    if (!require(!store.get(role_id), "owner should be cleared")) {
        return 7;
    }

    WorldMsgContext world{ServiceAddress{{1, 1, GameServiceType::world, 1, 1}, 400, yuan::game_base::ServerRole::world, 1, "world"}};
    world.ownership_store = std::make_shared<InMemoryWorldOwnershipStore>();
    if (!require(world_set_player_zone(world, role_id, zone_a, zone_a, 10), "world should set owner through store")) {
        return 8;
    }
    if (!require(world_set_player_zone(world, role_id, zone_b, zone_b, 20), "world should update owner through store")) {
        return 9;
    }
    if (!require(world_set_player_zone(world, role_id, 0, zone_a, 10), "world should ignore stale old-zone logout")) {
        return 10;
    }
    if (!require(world_player_zone(world, role_id).value_or(0) == zone_b, "world stale logout must not clear local owner")) {
        return 11;
    }
    return EXIT_SUCCESS;
}
