#ifndef YUAN_GAME_SERVER_COMMON_STORAGE_ENTITY_STORE_H
#define YUAN_GAME_SERVER_COMMON_STORAGE_ENTITY_STORE_H

#include "common/storage/orm.h"

#include "yuan/rpc/types.h"

#include <optional>

namespace yuan::game::server::storage
{
    struct EntityRecord
    {
        std::string table;
        std::string key;
        OrmFields fields;
        std::uint64_t version = 0;
        yuan::rpc::Bytes object_blob;
    };

    class EntityStore
    {
    public:
        explicit EntityStore(OrmStore &orm);

        [[nodiscard]] std::optional<EntityRecord> load(std::string table, std::string key);
        OrmResult insert(EntityRecord record);
        OrmResult save(EntityRecord record);
        OrmResult compare_and_save(EntityRecord record, std::uint64_t expected_version);
        OrmResult remove(std::string table, std::string key);
        std::vector<OrmResult> save_batch(std::vector<EntityRecord> records, bool transactional = false);

    private:
        OrmStore &orm_;
    };

    std::uint64_t field_u64(const OrmFields &fields, const std::string &name, std::uint64_t fallback = 0);
    std::uint32_t field_u32(const OrmFields &fields, const std::string &name, std::uint32_t fallback = 0);
    std::string bytes_to_field(const yuan::rpc::Bytes &bytes);
    yuan::rpc::Bytes field_to_bytes(const std::string &value);
}

#endif
