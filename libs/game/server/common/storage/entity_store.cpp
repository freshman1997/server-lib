#include "common/storage/entity_store.h"

namespace yuan::game::server::storage
{
    EntityStore::EntityStore(OrmStore &orm)
        : orm_(orm)
    {
    }

    std::optional<EntityRecord> EntityStore::load(std::string table, std::string key)
    {
        auto result = orm_.query(table, key, 1);
        if (!result.ok || result.rows.empty()) {
            return std::nullopt;
        }
        
        EntityRecord record;
        record.table = std::move(table);
        record.key = std::move(key);
        record.fields = std::move(result.rows.front().fields);
        if (auto it = record.fields.find("object_blob"); it != record.fields.end()) {
            record.object_blob = field_to_bytes(it->second);
        }

        record.version = field_u64(record.fields, "data_version", 0);
        return record;
    }

    OrmResult EntityStore::insert(EntityRecord record)
    {
        if (record.version == 0) {
            record.version = 1;
        }

        if (!record.object_blob.empty()) {
            record.fields["object_blob"] = bytes_to_field(record.object_blob);
        }

        record.fields["data_version"] = std::to_string(record.version);
        return orm_.insert(std::move(record.table), std::move(record.key), std::move(record.fields));
    }

    OrmResult EntityStore::save(EntityRecord record)
    {
        if (record.version == 0) {
            record.version = 1;
        }

        if (!record.object_blob.empty()) {
            record.fields["object_blob"] = bytes_to_field(record.object_blob);
        }

        record.fields["data_version"] = std::to_string(record.version);
        return orm_.upsert(std::move(record.table), std::move(record.key), std::move(record.fields));
    }

    OrmResult EntityStore::compare_and_save(EntityRecord record, std::uint64_t expected_version)
    {
        if (record.version == 0) {
            record.version = expected_version + 1;
        }

        if (!record.object_blob.empty()) {
            record.fields["object_blob"] = bytes_to_field(record.object_blob);
        }

        record.fields["data_version"] = std::to_string(record.version);
        return orm_.compare_and_update(std::move(record.table), std::move(record.key), "data_version", expected_version, std::move(record.fields));
    }

    OrmResult EntityStore::remove(std::string table, std::string key)
    {
        return orm_.delete_(std::move(table), std::move(key));
    }

    std::vector<OrmResult> EntityStore::save_batch(std::vector<EntityRecord> records, bool transactional)
    {
        std::vector<DbOrmOperation> operations;
        operations.reserve(records.size());
        for (auto &record : records) {
            if (record.version == 0) {
                record.version = 1;
            }

            if (!record.object_blob.empty()) {
                record.fields["object_blob"] = bytes_to_field(record.object_blob);
            }

            record.fields["data_version"] = std::to_string(record.version);
            operations.push_back(DbOrmOperation{static_cast<std::uint32_t>(DbOrmOpType::update),
                                                std::move(record.table),
                                                std::move(record.key),
                                                fields_to_proto(record.fields),
                                                0});
        }

        return orm_.batch(operations, transactional);
    }

    std::uint64_t field_u64(const OrmFields &fields, const std::string &name, std::uint64_t fallback)
    {
        const auto it = fields.find(name);
        if (it == fields.end()) {
            return fallback;
        }

        try {
            return static_cast<std::uint64_t>(std::stoull(it->second));
        } catch (...) {
            return fallback;
        }
    }

    std::uint32_t field_u32(const OrmFields &fields, const std::string &name, std::uint32_t fallback)
    {
        return static_cast<std::uint32_t>(field_u64(fields, name, fallback));
    }

    std::string bytes_to_field(const yuan::rpc::Bytes &bytes)
    {
        return std::string(reinterpret_cast<const char *>(bytes.data()), bytes.size());
    }

    yuan::rpc::Bytes field_to_bytes(const std::string &value)
    {
        return yuan::rpc::Bytes(value.begin(), value.end());
    }
}
