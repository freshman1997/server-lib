#include "../redis_impl.h"
#include "../utils.h"
#include "cmd/pipeline_cmd.h"
#include "internal/cmd_builder.h"
#include "redis_client.h"
#include "value/array_value.h"
#include "value/error_value.h"

#include <cctype>
#include <tuple>
#include <vector>

namespace yuan::redis
{
    namespace
    {
        std::shared_ptr<StringValue> str_arg(const std::string &value)
        {
            return std::make_shared<StringValue>(value);
        }

        std::shared_ptr<StringValue> int_arg(const int64_t value)
        {
            return std::make_shared<StringValue>(std::to_string(value));
        }

        std::shared_ptr<StringValue> float_arg(const double value)
        {
            return std::make_shared<StringValue>(serializeDouble(value));
        }

        std::vector<std::string> split_command_line(const std::string &command)
        {
            std::vector<std::string> tokens;
            std::string current;
            char quote = 0;
            bool escaping = false;

            for (const char ch : command) {
                if (escaping) {
                    current.push_back(ch);
                    escaping = false;
                    continue;
                }

                if (ch == '\\') {
                    escaping = true;
                    continue;
                }

                if (quote != 0) {
                    if (ch == quote) {
                        quote = 0;
                    } else {
                        current.push_back(ch);
                    }
                    continue;
                }

                if (ch == '"' || ch == '\'') {
                    quote = ch;
                    continue;
                }

                if (std::isspace(static_cast<unsigned char>(ch))) {
                    if (!current.empty()) {
                        tokens.push_back(current);
                        current.clear();
                    }
                    continue;
                }

                current.push_back(ch);
            }

            if (!current.empty()) {
                tokens.push_back(current);
            }

            return tokens;
        }

        std::shared_ptr<DefaultCmd> parse_raw_command(const std::string &command)
        {
            const auto tokens = split_command_line(command);
            if (tokens.empty()) {
                return nullptr;
            }

            auto cmd = make_cmd(tokens.front());
            for (std::size_t i = 1; i < tokens.size(); ++i) {
                cmd->add_arg(str_arg(tokens[i]));
            }

            return cmd;
        }

        bool append_streams_clause(DefaultCmd &cmd, const std::vector<std::string> &streams)
        {
            if (streams.empty() || streams.size() % 2 != 0) {
                return false;
            }

            const std::size_t split = streams.size() / 2;
            cmd.add_arg(str_arg("STREAMS"));
            for (std::size_t i = 0; i < split; ++i) {
                cmd.add_arg(str_arg(streams[i]));
            }
            for (std::size_t i = split; i < streams.size(); ++i) {
                cmd.add_arg(str_arg(streams[i]));
            }

            return true;
        }

    }

    std::shared_ptr<RedisValue> RedisClient::discard()
    {
        std::lock_guard<std::recursive_mutex> lock(impl_->operation_mutex_);
        auto cmd = make_cmd("discard");
        impl_->multi_cmd_ = nullptr;
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::watch(const std::vector<std::string> &keys)
    {
        auto cmd = make_cmd("watch");
        for (const auto &key : keys) {
            cmd->add_arg(str_arg(key));
        }
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::unwatch()
    {
        auto cmd = make_cmd("unwatch");
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::multi_exec(const std::vector<std::string> &commands)
    {
        if (!multi()) {
            return ErrorValue::from_string("ERR: MULTI already active");
        }

        for (const auto &raw_command : commands) {
            const auto cmd = parse_raw_command(raw_command);
            if (!cmd) {
                impl_->multi_cmd_ = nullptr;
                return ErrorValue::from_string("ERR: invalid command in MULTI");
            }
            impl_->multi_cmd_->add_command(cmd);
        }

        return exec();
    }

    std::shared_ptr<RedisValue> RedisClient::multi_exec(const std::vector<std::string> &commands, const std::vector<std::string> &keys)
    {
        const auto watch_result = watch(keys);
        if (!watch_result && get_last_error()) {
            return nullptr;
        }

        return multi_exec(commands);
    }

    std::shared_ptr<RedisValue> RedisClient::multi_exec(
        const std::vector<std::string> &commands,
        const std::vector<std::string> &keys,
        const std::vector<std::string> &unwatch_keys)
    {
        const auto result = multi_exec(commands, keys);
        if (!unwatch_keys.empty()) {
            (void)unwatch();
        }
        return result;
    }

    std::shared_ptr<RedisValue> RedisClient::pipeline(const std::vector<std::string> &commands)
    {
        if (commands.empty()) {
            auto arr = std::make_shared<ArrayValue>();
            arr->set_values({});
            return arr;
        }

        auto pipeline_cmd = std::make_shared<PipelineCmd>();
        for (const auto &raw_command : commands) {
            const auto cmd = parse_raw_command(raw_command);
            if (!cmd) {
                return ErrorValue::from_string("ERR: invalid command in PIPELINE");
            }
            pipeline_cmd->add_command(cmd);
        }

        return impl_->execute_command(pipeline_cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::pipeline(const std::vector<PipelineCommand> &commands)
    {
        if (commands.empty()) {
            auto arr = std::make_shared<ArrayValue>();
            arr->set_values({});
            return arr;
        }

        auto pipeline_cmd = std::make_shared<PipelineCmd>();
        for (const auto &command : commands) {
            if (command.name.empty()) {
                return ErrorValue::from_string("ERR: invalid command in PIPELINE");
            }

            auto cmd = make_cmd(command.name);
            for (const auto &arg : command.args) {
                cmd->add_arg(str_arg(arg));
            }
            pipeline_cmd->add_command(cmd);
        }

        return impl_->execute_command(pipeline_cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::setbit(std::string key, int64_t offset, int value)
    {
        auto cmd = make_cmd("setbit", {str_arg(key), int_arg(offset), int_arg(value)});
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::getbit(std::string key, int64_t offset)
    {
        auto cmd = make_cmd("getbit", {str_arg(key), int_arg(offset)});
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::bitcount(std::string key, int64_t start, int64_t end)
    {
        auto cmd = make_cmd("bitcount", {str_arg(key)});
        if (start != 0 || end != -1) {
            cmd->add_arg(int_arg(start));
            cmd->add_arg(int_arg(end));
        }
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::bitop_and(std::string destkey, const std::vector<std::string> &keys)
    {
        auto cmd = make_cmd("bitop", {str_arg("AND"), str_arg(destkey)});
        for (const auto &key : keys) {
            cmd->add_arg(str_arg(key));
        }
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::bitop_or(std::string destkey, const std::vector<std::string> &keys)
    {
        auto cmd = make_cmd("bitop", {str_arg("OR"), str_arg(destkey)});
        for (const auto &key : keys) {
            cmd->add_arg(str_arg(key));
        }
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::bitop_xor(std::string destkey, const std::vector<std::string> &keys)
    {
        auto cmd = make_cmd("bitop", {str_arg("XOR"), str_arg(destkey)});
        for (const auto &key : keys) {
            cmd->add_arg(str_arg(key));
        }
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::bitop_not(std::string destkey, std::string key)
    {
        auto cmd = make_cmd("bitop", {str_arg("NOT"), str_arg(destkey), str_arg(key)});
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::bitpos(std::string key, int bit, int64_t start, int64_t end)
    {
        auto cmd = make_cmd("bitpos", {str_arg(key), int_arg(bit)});
        if (start != 0 || end != -1) {
            cmd->add_arg(int_arg(start));
            if (end != -1) {
                cmd->add_arg(int_arg(end));
            }
        }
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::bitfield(std::string key, const std::vector<std::string> &subcommands)
    {
        auto cmd = make_cmd("bitfield", {str_arg(key)});
        for (const auto &subcommand : subcommands) {
            cmd->add_arg(str_arg(subcommand));
        }
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::bitfield_ro(std::string key, const std::vector<std::string> &subcommands)
    {
        auto cmd = make_cmd("bitfield_ro", {str_arg(key)});
        for (const auto &subcommand : subcommands) {
            cmd->add_arg(str_arg(subcommand));
        }
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::bitfield_rw(std::string key, const std::vector<std::string> &subcommands)
    {
        return bitfield(std::move(key), subcommands);
    }

    std::shared_ptr<RedisValue> RedisClient::bitfield_incrby(std::string key, int64_t offset, int64_t increment)
    {
        auto cmd = make_cmd("bitfield", {str_arg(key), str_arg("INCRBY"), str_arg("i64"), int_arg(offset), int_arg(increment)});
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::bitfield_incrby_i64_with_overflow(
        std::string key,
        int64_t offset,
        int64_t increment,
        std::string overflow)
    {
        auto cmd = make_cmd("bitfield", {
            str_arg(key),
            str_arg("OVERFLOW"),
            str_arg(overflow),
            str_arg("INCRBY"),
            str_arg("i64"),
            int_arg(offset),
            int_arg(increment)
        });
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::bitfield_set_i64(std::string key, int64_t offset, std::string value)
    {
        auto cmd = make_cmd("bitfield", {str_arg(key), str_arg("SET"), str_arg("i64"), int_arg(offset), str_arg(value)});
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::bitfield_get(std::string key, int64_t offset, int64_t length)
    {
        auto cmd = make_cmd("bitfield", {str_arg(key), str_arg("GET"), str_arg("u" + std::to_string(length)), int_arg(offset)});
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::bitfield_set(std::string key, int64_t offset, std::string value)
    {
        auto cmd = make_cmd("bitfield", {str_arg(key), str_arg("SET"), str_arg("i64"), int_arg(offset), str_arg(value)});
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::geoadd(std::string key, const std::vector<std::tuple<double, double, std::string>> &members)
    {
        auto cmd = make_cmd("geoadd", {str_arg(key)});
        for (const auto &[longitude, latitude, member] : members) {
            cmd->add_arg(float_arg(longitude));
            cmd->add_arg(float_arg(latitude));
            cmd->add_arg(str_arg(member));
        }
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::geodist(std::string key, std::string member1, std::string member2, const std::string &unit)
    {
        auto cmd = make_cmd("geodist", {str_arg(key), str_arg(member1), str_arg(member2)});
        if (!unit.empty()) {
            cmd->add_arg(str_arg(unit));
        }
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::geohash(std::string key, const std::vector<std::string> &members)
    {
        auto cmd = make_cmd("geohash", {str_arg(key)});
        for (const auto &member : members) {
            cmd->add_arg(str_arg(member));
        }
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::geopos(std::string key, const std::vector<std::string> &members)
    {
        auto cmd = make_cmd("geopos", {str_arg(key)});
        for (const auto &member : members) {
            cmd->add_arg(str_arg(member));
        }
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::georadius(
        std::string key,
        double longitude,
        double latitude,
        double radius,
        const std::string &unit,
        int64_t count,
        bool with_coord,
        bool with_dist,
        bool with_hash,
        const std::string &sort)
    {
        auto cmd = make_cmd("georadius", {str_arg(key), float_arg(longitude), float_arg(latitude), float_arg(radius)});
        if (!unit.empty()) {
            cmd->add_arg(str_arg(unit));
        }
        if (with_coord) {
            cmd->add_arg(str_arg("WITHCOORD"));
        }
        if (with_dist) {
            cmd->add_arg(str_arg("WITHDIST"));
        }
        if (with_hash) {
            cmd->add_arg(str_arg("WITHHASH"));
        }
        if (count > 0) {
            cmd->add_arg(str_arg("COUNT"));
            cmd->add_arg(int_arg(count));
        }
        if (!sort.empty()) {
            cmd->add_arg(str_arg(sort));
        }
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::georadiusbymember(
        std::string key,
        std::string member,
        double radius,
        const std::string &unit,
        int64_t count,
        bool with_coord,
        bool with_dist,
        bool with_hash,
        const std::string &sort)
    {
        auto cmd = make_cmd("georadiusbymember", {str_arg(key), str_arg(member), float_arg(radius)});
        if (!unit.empty()) {
            cmd->add_arg(str_arg(unit));
        }
        if (with_coord) {
            cmd->add_arg(str_arg("WITHCOORD"));
        }
        if (with_dist) {
            cmd->add_arg(str_arg("WITHDIST"));
        }
        if (with_hash) {
            cmd->add_arg(str_arg("WITHHASH"));
        }
        if (count > 0) {
            cmd->add_arg(str_arg("COUNT"));
            cmd->add_arg(int_arg(count));
        }
        if (!sort.empty()) {
            cmd->add_arg(str_arg(sort));
        }
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::georadiusbymember(
        std::string key,
        std::string member,
        double radius,
        const std::string &unit,
        int64_t count,
        bool with_coord,
        bool with_dist,
        bool with_hash,
        const std::string &sort,
        std::string store_key)
    {
        auto cmd = make_cmd("georadiusbymember", {str_arg(key), str_arg(member), float_arg(radius)});
        if (!unit.empty()) {
            cmd->add_arg(str_arg(unit));
        }
        if (with_coord) {
            cmd->add_arg(str_arg("WITHCOORD"));
        }
        if (with_dist) {
            cmd->add_arg(str_arg("WITHDIST"));
        }
        if (with_hash) {
            cmd->add_arg(str_arg("WITHHASH"));
        }
        if (count > 0) {
            cmd->add_arg(str_arg("COUNT"));
            cmd->add_arg(int_arg(count));
        }
        if (!sort.empty()) {
            cmd->add_arg(str_arg(sort));
        }
        if (!store_key.empty()) {
            cmd->add_arg(str_arg("STORE"));
            cmd->add_arg(str_arg(store_key));
        }
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::georadius(
        std::string key,
        double longitude,
        double latitude,
        double radius,
        const std::string &unit,
        int64_t count,
        bool with_coord,
        bool with_dist,
        bool with_hash,
        const std::string &sort,
        std::string store_key)
    {
        auto cmd = make_cmd("georadius", {str_arg(key), float_arg(longitude), float_arg(latitude), float_arg(radius)});
        if (!unit.empty()) {
            cmd->add_arg(str_arg(unit));
        }
        if (with_coord) {
            cmd->add_arg(str_arg("WITHCOORD"));
        }
        if (with_dist) {
            cmd->add_arg(str_arg("WITHDIST"));
        }
        if (with_hash) {
            cmd->add_arg(str_arg("WITHHASH"));
        }
        if (count > 0) {
            cmd->add_arg(str_arg("COUNT"));
            cmd->add_arg(int_arg(count));
        }
        if (!sort.empty()) {
            cmd->add_arg(str_arg(sort));
        }
        if (!store_key.empty()) {
            cmd->add_arg(str_arg("STORE"));
            cmd->add_arg(str_arg(store_key));
        }
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::shutdown(std::string save, std::string nopersist)
    {
        auto cmd = make_cmd("shutdown", {str_arg(save), str_arg(nopersist)});
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::shutdown(std::string save, std::string nopersist, std::string force)
    {
        auto cmd = make_cmd("shutdown", {str_arg(save), str_arg(nopersist), str_arg(force)});
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::shutdown(std::string save, std::string nopersist, std::string force, std::string noflush)
    {
        auto cmd = make_cmd("shutdown", {str_arg(save), str_arg(nopersist), str_arg(force), str_arg(noflush)});
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::shutdown(std::string save, std::string nopersist, std::string force, std::string noflush, std::string async)
    {
        auto cmd = make_cmd("shutdown", {str_arg(save), str_arg(nopersist), str_arg(force), str_arg(noflush), str_arg(async)});
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::shutdown(std::string save, std::string nopersist, std::string force, std::string noflush, std::string async, std::string skip_slave_start)
    {
        auto cmd = make_cmd("shutdown", {str_arg(save), str_arg(nopersist), str_arg(force), str_arg(noflush), str_arg(async), str_arg(skip_slave_start)});
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::shutdown(
        std::string save,
        std::string nopersist,
        std::string force,
        std::string noflush,
        std::string async,
        std::string skip_slave_start,
        std::string no_save)
    {
        auto cmd = make_cmd("shutdown", {str_arg(save), str_arg(nopersist), str_arg(force), str_arg(noflush), str_arg(async), str_arg(skip_slave_start), str_arg(no_save)});
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::xack(std::string key, std::string group, const std::vector<std::string> &ids)
    {
        auto cmd = make_cmd("xack", {str_arg(key), str_arg(group)});
        for (const auto &id : ids) {
            cmd->add_arg(str_arg(id));
        }
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::xadd(std::string key, std::string id, const std::vector<std::string> &fields)
    {
        if (fields.empty() || fields.size() % 2 != 0) {
            set_last_error(ErrorValue::from_string("ERR: XADD fields must be key/value pairs"));
            return nullptr;
        }

        auto cmd = make_cmd("xadd", {str_arg(key), str_arg(id)});
        for (const auto &field : fields) {
            cmd->add_arg(str_arg(field));
        }
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::xclaim(std::string key, std::string group, std::string consumer, int64_t min_idle_time, const std::vector<std::string> &ids)
    {
        auto cmd = make_cmd("xclaim", {str_arg(key), str_arg(group), str_arg(consumer), int_arg(min_idle_time)});
        for (const auto &id : ids) {
            cmd->add_arg(str_arg(id));
        }
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::xclaim(
        std::string key,
        std::string group,
        std::string consumer,
        int64_t min_idle_time,
        const std::vector<std::string> &ids,
        int64_t idle_time)
    {
        auto cmd = make_cmd("xclaim", {str_arg(key), str_arg(group), str_arg(consumer), int_arg(min_idle_time)});
        for (const auto &id : ids) {
            cmd->add_arg(str_arg(id));
        }
        cmd->add_arg(str_arg("IDLE"));
        cmd->add_arg(int_arg(idle_time));
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::xclaim(
        std::string key,
        std::string group,
        std::string consumer,
        int64_t min_idle_time,
        const std::vector<std::string> &ids,
        int64_t idle_time,
        bool just_id)
    {
        auto cmd = make_cmd("xclaim", {str_arg(key), str_arg(group), str_arg(consumer), int_arg(min_idle_time)});
        for (const auto &id : ids) {
            cmd->add_arg(str_arg(id));
        }
        cmd->add_arg(str_arg("IDLE"));
        cmd->add_arg(int_arg(idle_time));
        if (just_id) {
            cmd->add_arg(str_arg("JUSTID"));
        }
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::xdel(std::string key, const std::vector<std::string> &ids)
    {
        auto cmd = make_cmd("xdel", {str_arg(key)});
        for (const auto &id : ids) {
            cmd->add_arg(str_arg(id));
        }
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::xgroup_create(std::string key, std::string group, std::string id, bool mkstream)
    {
        auto cmd = make_cmd("xgroup", {str_arg("CREATE"), str_arg(key), str_arg(group), str_arg(id)});
        if (mkstream) {
            cmd->add_arg(str_arg("MKSTREAM"));
        }
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::xgroup_setid(std::string key, std::string group, std::string id)
    {
        auto cmd = make_cmd("xgroup", {str_arg("SETID"), str_arg(key), str_arg(group), str_arg(id)});
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::xgroup_destroy(std::string key, std::string group)
    {
        auto cmd = make_cmd("xgroup", {str_arg("DESTROY"), str_arg(key), str_arg(group)});
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::xgroup_delconsumer(std::string key, std::string group, std::string consumer)
    {
        auto cmd = make_cmd("xgroup", {str_arg("DELCONSUMER"), str_arg(key), str_arg(group), str_arg(consumer)});
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::xinfo_stream(std::string key)
    {
        auto cmd = make_cmd("xinfo", {str_arg("STREAM"), str_arg(key)});
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::xinfo_groups(std::string key)
    {
        auto cmd = make_cmd("xinfo", {str_arg("GROUPS"), str_arg(key)});
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::xinfo_consumers(std::string key, std::string group)
    {
        auto cmd = make_cmd("xinfo", {str_arg("CONSUMERS"), str_arg(key), str_arg(group)});
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::xlen(std::string key)
    {
        auto cmd = make_cmd("xlen", {str_arg(key)});
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::xpending(std::string key, std::string group)
    {
        auto cmd = make_cmd("xpending", {str_arg(key), str_arg(group)});
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::xpending(std::string key, std::string group, std::string start, std::string end, int count, std::string consumer)
    {
        auto cmd = make_cmd("xpending", {str_arg(key), str_arg(group), str_arg(start), str_arg(end), int_arg(count), str_arg(consumer)});
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::xrange(std::string key, std::string start, std::string end)
    {
        auto cmd = make_cmd("xrange", {str_arg(key), str_arg(start), str_arg(end)});
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::xrange(std::string key, std::string start, std::string end, int count)
    {
        auto cmd = make_cmd("xrange", {str_arg(key), str_arg(start), str_arg(end), str_arg("COUNT"), int_arg(count)});
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::xread(const std::vector<std::string> &streams)
    {
        auto cmd = make_cmd("xread");
        if (!append_streams_clause(*cmd, streams)) {
            set_last_error(ErrorValue::from_string("ERR: XREAD requires stream/id pairs"));
            return nullptr;
        }
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::xread(const std::vector<std::string> &streams, int count)
    {
        auto cmd = make_cmd("xread", {str_arg("COUNT"), int_arg(count)});
        if (!append_streams_clause(*cmd, streams)) {
            set_last_error(ErrorValue::from_string("ERR: XREAD requires stream/id pairs"));
            return nullptr;
        }
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::xread(const std::vector<std::string> &streams, int count, bool block)
    {
        return xread(streams, count, block, 0);
    }

    std::shared_ptr<RedisValue> RedisClient::xread(const std::vector<std::string> &streams, int count, bool block, int block_time)
    {
        auto cmd = make_cmd("xread");
        if (count > 0) {
            cmd->add_arg(str_arg("COUNT"));
            cmd->add_arg(int_arg(count));
        }
        if (block) {
            cmd->add_arg(str_arg("BLOCK"));
            cmd->add_arg(int_arg(block_time));
        }
        if (!append_streams_clause(*cmd, streams)) {
            set_last_error(ErrorValue::from_string("ERR: XREAD requires stream/id pairs"));
            return nullptr;
        }
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::xreadgroup(std::string group, std::string consumer, const std::vector<std::string> &streams)
    {
        auto cmd = make_cmd("xreadgroup", {str_arg("GROUP"), str_arg(group), str_arg(consumer)});
        if (!append_streams_clause(*cmd, streams)) {
            set_last_error(ErrorValue::from_string("ERR: XREADGROUP requires stream/id pairs"));
            return nullptr;
        }
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::xreadgroup(std::string group, std::string consumer, const std::vector<std::string> &streams, int count)
    {
        auto cmd = make_cmd("xreadgroup", {str_arg("GROUP"), str_arg(group), str_arg(consumer), str_arg("COUNT"), int_arg(count)});
        if (!append_streams_clause(*cmd, streams)) {
            set_last_error(ErrorValue::from_string("ERR: XREADGROUP requires stream/id pairs"));
            return nullptr;
        }
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::xreadgroup(std::string group, std::string consumer, const std::vector<std::string> &streams, int count, bool block)
    {
        return xreadgroup(group, consumer, streams, count, block, 0);
    }

    std::shared_ptr<RedisValue> RedisClient::xreadgroup(
        std::string group,
        std::string consumer,
        const std::vector<std::string> &streams,
        int count,
        bool block,
        int block_time)
    {
        auto cmd = make_cmd("xreadgroup", {str_arg("GROUP"), str_arg(group), str_arg(consumer)});
        if (count > 0) {
            cmd->add_arg(str_arg("COUNT"));
            cmd->add_arg(int_arg(count));
        }
        if (block) {
            cmd->add_arg(str_arg("BLOCK"));
            cmd->add_arg(int_arg(block_time));
        }
        if (!append_streams_clause(*cmd, streams)) {
            set_last_error(ErrorValue::from_string("ERR: XREADGROUP requires stream/id pairs"));
            return nullptr;
        }
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::xrevrange(std::string key, std::string end, std::string start)
    {
        auto cmd = make_cmd("xrevrange", {str_arg(key), str_arg(end), str_arg(start)});
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::xrevrange(std::string key, std::string end, std::string start, int count)
    {
        auto cmd = make_cmd("xrevrange", {str_arg(key), str_arg(end), str_arg(start), str_arg("COUNT"), int_arg(count)});
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::xtrim(std::string key, int64_t max_len, bool approximate)
    {
        auto cmd = make_cmd("xtrim", {str_arg(key), str_arg("MAXLEN"), str_arg(approximate ? "~" : "="), int_arg(max_len)});
        return impl_->execute_command(cmd);
    }
}
