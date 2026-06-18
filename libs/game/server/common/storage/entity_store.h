#ifndef YUAN_GAME_SERVER_COMMON_STORAGE_ENTITY_STORE_H
#define YUAN_GAME_SERVER_COMMON_STORAGE_ENTITY_STORE_H

#include "common/storage/orm.h"

#include <optional>

namespace yuan::game::server::storage
{
    struct EntityRecord
    {
        std::string table;
        std::string key;
        OrmFields fields;
        std::uint64_t version = 0;
    };

    class EntityStore
    {
    public:
        explicit EntityStore(OrmStore &orm);

        [[nodiscard]] std::optional<EntityRecord> load(std::string table, std::string key);
        OrmResult save(EntityRecord record);
        OrmResult remove(std::string table, std::string key);
        std::vector<OrmResult> save_batch(std::vector<EntityRecord> records, bool transactional = false);

    private:
        OrmStore &orm_;
    };

    std::uint64_t field_u64(const OrmFields &fields, const std::string &name, std::uint64_t fallback = 0);
    std::uint32_t field_u32(const OrmFields &fields, const std::string &name, std::uint32_t fallback = 0);
}

#endif
