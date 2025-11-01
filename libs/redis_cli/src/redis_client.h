#ifndef __YUAN_REDIS_CLIENT_H__
#define __YUAN_REDIS_CLIENT_H__
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "option.h"
#include "redis_value.h"

namespace yuan::redis 
{
    class RedisClient 
    {
    public:
        RedisClient() = default;

        RedisClient(const Option &opt);
        
        ~RedisClient();

    public:
        void set_option(const Option &opt);

        bool is_connected() const;

        void disconnect();

        std::shared_ptr<RedisValue> get_last_error() const;

    public: // common commands
        // string
        std::shared_ptr<RedisValue> get(std::string key);
        std::shared_ptr<RedisValue> set(std::string key, std::string value);
        std::shared_ptr<RedisValue> set(std::string key, std::string value, int expire);
        std::shared_ptr<RedisValue> set(std::string key, std::string value, int expire, int nx);
        std::shared_ptr<RedisValue> set(std::string key, std::string value, int expire, int nx, int xx);

        // hash
        std::shared_ptr<RedisValue> hget(std::string key, std::string field);
        std::shared_ptr<RedisValue> hset(std::string key, std::string field, std::string value);
        std::shared_ptr<RedisValue> hset(std::string key, const std::unordered_map<std::string, std::string> &field_values);
        std::shared_ptr<RedisValue> hmset(std::string key, const std::unordered_map<std::string, std::string> &field_values);
        std::shared_ptr<RedisValue> hmget(std::string key, const std::vector<std::string> &fields);
        std::shared_ptr<RedisValue> hgetall(std::string key);
        std::shared_ptr<RedisValue> hdel(std::string key, const std::vector<std::string> &fields);
        std::shared_ptr<RedisValue> hlen(std::string key);
        std::shared_ptr<RedisValue> hkeys(std::string key);
        std::shared_ptr<RedisValue> hvals(std::string key);
        std::shared_ptr<RedisValue> hincrby(std::string key, std::string field, int64_t increment);
        std::shared_ptr<RedisValue> hincrbyfloat(std::string key, std::string field, double increment);
        std::shared_ptr<RedisValue> hexists(std::string key, std::string field);

        // list
        std::shared_ptr<RedisValue> lpush(std::string key, const std::vector<std::string> &values);
        std::shared_ptr<RedisValue> rpush(std::string key, const std::vector<std::string> &values);
        std::shared_ptr<RedisValue> lpop(std::string key);
        std::shared_ptr<RedisValue> rpop(std::string key);
        std::shared_ptr<RedisValue> lrange(std::string key, int64_t start, int64_t stop);
        std::shared_ptr<RedisValue> lindex(std::string key, int64_t index);
        std::shared_ptr<RedisValue> llen(std::string key);
        std::shared_ptr<RedisValue> lset(std::string key, int64_t index, std::string value);
        std::shared_ptr<RedisValue> lrem(std::string key, int64_t count, std::string value);
        std::shared_ptr<RedisValue> ltrim(std::string key, int64_t start, int64_t stop);
        std::shared_ptr<RedisValue> linsert(std::string key, std::string pivot, std::string value, bool before);
        std::shared_ptr<RedisValue> linsert(std::string key, std::string pivot, const std::vector<std::string> &values, bool before);
        std::shared_ptr<RedisValue> rpoplpush(std::string source, std::string destination);
        std::shared_ptr<RedisValue> brpoplpush(std::string source, std::string destination, int timeout);

        // set
        std::shared_ptr<RedisValue> sadd(std::string key, const std::vector<std::string> &members);
        std::shared_ptr<RedisValue> srem(std::string key, const std::vector<std::string> &members);
        std::shared_ptr<RedisValue> smembers(std::string key);
        std::shared_ptr<RedisValue> sismember(std::string key, std::string member);
        std::shared_ptr<RedisValue> scard(std::string key);
        std::shared_ptr<RedisValue> srandmember(std::string key);
        std::shared_ptr<RedisValue> srandmember(std::string key, int count);
        std::shared_ptr<RedisValue> spop(std::string key);
        std::shared_ptr<RedisValue> spop(std::string key, int count);
        std::shared_ptr<RedisValue> smove(std::string source, std::string destination, std::string member);
        std::shared_ptr<RedisValue> sscan(std::string key, int64_t cursor, const std::string &match_pattern = "", int64_t count = 10);
        std::shared_ptr<RedisValue> sscan(std::string key, int64_t cursor, const std::vector<std::string> &match_patterns, int64_t count = 10);
        std::shared_ptr<RedisValue> sdiff(const std::vector<std::string> &keys);
        std::shared_ptr<RedisValue> sdiffstore(std::string destination, const std::vector<std::string> &keys);
        std::shared_ptr<RedisValue> sinter(const std::vector<std::string> &keys);
        std::shared_ptr<RedisValue> sinterstore(std::string destination, const std::vector<std::string> &keys);
        std::shared_ptr<RedisValue> sunion(const std::vector<std::string> &keys);
        std::shared_ptr<RedisValue> sunionstore(std::string destination, const std::vector<std::string> &keys);

        // zset
        std::shared_ptr<RedisValue> zadd(std::string key, const std::unordered_map<std::string, double> &member_scores, bool nx = false, bool xx = false, bool ch = false, bool incr = false);
        std::shared_ptr<RedisValue> zrem(std::string key, const std::vector<std::string> &members);
        std::shared_ptr<RedisValue> zrange(std::string key, int64_t start, int64_t stop, bool with_scores = false);
        std::shared_ptr<RedisValue> zrevrange(std::string key, int64_t start, int64_t stop, bool with_scores = false);
        std::shared_ptr<RedisValue> zrangebyscore(std::string key, double min, double max, bool with_scores = false, bool min_open = false, bool max_open = false, int64_t offset = 0, int64_t count = 10);
        std::shared_ptr<RedisValue> zrevrangebyscore(std::string key, double max, double min, bool with_scores = false, bool min_open = false, bool max_open = false, int64_t offset = 0, int64_t count = 10);
        std::shared_ptr<RedisValue> zrank(std::string key, std::string member);
        std::shared_ptr<RedisValue> zrevrank(std::string key, std::string member);
        std::shared_ptr<RedisValue> zcard(std::string key);
        std::shared_ptr<RedisValue> zscore(std::string key, std::string member);
        std::shared_ptr<RedisValue> zincrby(std::string key, std::string member, double increment);
        std::shared_ptr<RedisValue> zcount(std::string key, double min, double max, bool min_open = false, bool max_open = false);
        std::shared_ptr<RedisValue> zremrangebyrank(std::string key, int64_t start, int64_t stop);
        std::shared_ptr<RedisValue> zremrangebyscore(std::string key, double min, double max, bool min_open = false, bool max_open = false);
        std::shared_ptr<RedisValue> zunionstore(std::string destination, const std::vector<std::string> &keys, const std::vector<double> &weights, const std::string &aggregate = "sum");
        std::shared_ptr<RedisValue> zinterstore(std::string destination, const std::vector<std::string> &keys, const std::vector<double> &weights, const std::string &aggregate = "sum");
        std::shared_ptr<RedisValue> zscan(std::string key, int64_t cursor, const std::string &match_pattern = "", int64_t count = 10);
        std::shared_ptr<RedisValue> zscan(std::string key, int64_t cursor, const std::vector<std::string> &match_patterns, int64_t count = 10);
        std::shared_ptr<RedisValue> zrangebylex(std::string key, std::string min, std::string max, bool min_open = false, bool max_open = false, int64_t offset = 0, int64_t count = 10);
        std::shared_ptr<RedisValue> zrevrangebylex(std::string key, std::string max, std::string min, bool min_open = false, bool max_open = false, int64_t offset = 0, int64_t count = 10);
        std::shared_ptr<RedisValue> zlexcount(std::string key, std::string min, std::string max, bool min_open = false, bool max_open = false);
        std::shared_ptr<RedisValue> zremrangebylex(std::string key, std::string min, std::string max, bool min_open = false, bool max_open = false);
        std::shared_ptr<RedisValue> zpopmin(std::string key, int64_t count);
        std::shared_ptr<RedisValue> zpopmax(std::string key, int64_t count);
        std::shared_ptr<RedisValue> bzpopmin(std::string key, int timeout, int64_t count);
        std::shared_ptr<RedisValue> bzpopmax(std::string key, int timeout, int64_t count);
        std::shared_ptr<RedisValue> zrandmember(std::string key, int count, bool with_scores = false);

        // bitmap
        std::shared_ptr<RedisValue> setbit(std::string key, int64_t offset, int value);
        std::shared_ptr<RedisValue> getbit(std::string key, int64_t offset);
        std::shared_ptr<RedisValue> bitcount(std::string key, int64_t start = 0, int64_t end = -1);
        std::shared_ptr<RedisValue> bitop_and(std::string destkey, const std::vector<std::string> &keys);
        std::shared_ptr<RedisValue> bitop_or(std::string destkey, const std::vector<std::string> &keys);
        std::shared_ptr<RedisValue> bitop_xor(std::string destkey, const std::vector<std::string> &keys);
        std::shared_ptr<RedisValue> bitop_not(std::string destkey, std::string key);
        std::shared_ptr<RedisValue> bitpos(std::string key, int bit, int64_t start = 0, int64_t end = -1);
        std::shared_ptr<RedisValue> bitfield(std::string key, const std::vector<std::string> &subcommands);
        std::shared_ptr<RedisValue> bitfield_ro(std::string key, const std::vector<std::string> &subcommands);
        std::shared_ptr<RedisValue> bitfield_rw(std::string key, const std::vector<std::string> &subcommands);
        std::shared_ptr<RedisValue> bitfield_incrby(std::string key, int64_t offset, int64_t increment);
        std::shared_ptr<RedisValue> bitfield_overflow(std::string key, int64_t offset, std::string overflow);
        std::shared_ptr<RedisValue> bitfield_append(std::string key, int64_t offset, std::string value);
        std::shared_ptr<RedisValue> bitfield_get(std::string key, int64_t offset, int64_t length);
        std::shared_ptr<RedisValue> bitfield_set(std::string key, int64_t offset, std::string value);

        // geo
        std::shared_ptr<RedisValue> geoadd(std::string key, const std::vector<std::tuple<double, double, std::string>> &members);
        std::shared_ptr<RedisValue> geodist(std::string key, std::string member1, std::string member2, const std::string &unit = "m");
        std::shared_ptr<RedisValue> geohash(std::string key, const std::vector<std::string> &members);
        std::shared_ptr<RedisValue> geopos(std::string key, const std::vector<std::string> &members);
        std::shared_ptr<RedisValue> georadius(std::string key, double longitude, double latitude, double radius, const std::string &unit = "m", int64_t count = 10, bool with_coord = false, bool with_dist = false, bool with_hash = false, const std::string &sort = "");
        std::shared_ptr<RedisValue> georadiusbymember(std::string key, std::string member, double radius, const std::string &unit = "m", int64_t count = 10, bool with_coord = false, bool with_dist = false, bool with_hash = false, const std::string &sort = "");
        std::shared_ptr<RedisValue> georadiusbymember(std::string key, std::string member, double radius, const std::string &unit, int64_t count, bool with_coord, bool with_dist, bool with_hash, const std::string &sort, std::string store_key);
        std::shared_ptr<RedisValue> georadius(std::string key, double longitude, double latitude, double radius, const std::string &unit, int64_t count, bool with_coord, bool with_dist, bool with_hash, const std::string &sort, std::string store_key);

        // pub/sub
        std::shared_ptr<RedisValue> publish(std::string channel, std::string message);
        std::shared_ptr<RedisValue> subscribe(const std::vector<std::string> &channels);
        std::shared_ptr<RedisValue> psubscribe(const std::vector<std::string> &patterns);
        std::shared_ptr<RedisValue> unsubscribe(const std::vector<std::string> &channels);
        std::shared_ptr<RedisValue> punsubscribe(const std::vector<std::string> &patterns);
        std::shared_ptr<RedisValue> psubscribe(const std::vector<std::string> &patterns, const std::vector<std::string> &channels);
        std::shared_ptr<RedisValue> unsubscribe(const std::vector<std::string> &channels, const std::vector<std::string> &patterns);
        std::shared_ptr<RedisValue> psubscribe(const std::vector<std::string> &patterns, const std::vector<std::string> &channels, const std::vector<std::string> &unsubscribe_patterns);
        std::shared_ptr<RedisValue> unsubscribe(const std::vector<std::string> &channels, const std::vector<std::string> &patterns, const std::vector<std::string> &unsubscribe_channels);

        // transaction
        bool multi();
        std::shared_ptr<RedisValue> exec();
        std::shared_ptr<RedisValue> discard();
        std::shared_ptr<RedisValue> watch(const std::vector<std::string> &keys);
        std::shared_ptr<RedisValue> unwatch();
        std::shared_ptr<RedisValue> multi_exec(const std::vector<std::string> &commands);
        std::shared_ptr<RedisValue> multi_exec(const std::vector<std::string> &commands, const std::vector<std::string> &keys);
        std::shared_ptr<RedisValue> multi_exec(const std::vector<std::string> &commands, const std::vector<std::string> &keys, const std::vector<std::string> &unwatch_keys);

        // scripting
        std::shared_ptr<RedisValue> eval(std::string script, const std::vector<std::string> &keys, const std::vector<std::string> &args);
        std::shared_ptr<RedisValue> evalsha(std::string sha1, const std::vector<std::string> &keys, const std::vector<std::string> &args);
        std::shared_ptr<RedisValue> script_load(std::string script);
        std::shared_ptr<RedisValue> script_exists(const std::string &sha1s);
        std::shared_ptr<RedisValue> script_exists(const std::vector<std::string> &sha1s);
        std::shared_ptr<RedisValue> script_flush();
        std::shared_ptr<RedisValue> script_kill();
        std::shared_ptr<RedisValue> script_flush(const std::vector<std::string> &keys);
        
    public: // special command
        // auth
        std::shared_ptr<RedisValue> auth(std::string password);

        // info
        std::shared_ptr<RedisValue> info(std::string section = "");

        // connect
        std::shared_ptr<RedisValue> ping();
        std::shared_ptr<RedisValue> echo(std::string message);
        std::shared_ptr<RedisValue> select(int index);
        std::shared_ptr<RedisValue> quit();
        std::shared_ptr<RedisValue> swapdb(int index1, int index2);
        std::shared_ptr<RedisValue> time();
        std::shared_ptr<RedisValue> wait(int numreplicas, int timeout);

        // server
        std::shared_ptr<RedisValue> bgrewriteaof();
        std::shared_ptr<RedisValue> bgsave();
        std::shared_ptr<RedisValue> client_getname();
        std::shared_ptr<RedisValue> client_id();
        std::shared_ptr<RedisValue> client_list();
        std::shared_ptr<RedisValue> client_pause(int timeout);
        std::shared_ptr<RedisValue> client_reply(std::string mode);
        std::shared_ptr<RedisValue> client_setname(std::string name);
        std::shared_ptr<RedisValue> client_unblock(int id);
        std::shared_ptr<RedisValue> command();
        std::shared_ptr<RedisValue> command_count();
        std::shared_ptr<RedisValue> command_getkeys();
        std::shared_ptr<RedisValue> command_info(const std::vector<std::string> &commands);
        std::shared_ptr<RedisValue> config_get(std::string parameter);
        std::shared_ptr<RedisValue> config_rewrite();
        std::shared_ptr<RedisValue> config_set(std::string parameter, std::string value);
        std::shared_ptr<RedisValue> config_resetstat();
        std::shared_ptr<RedisValue> dbsize();
        std::shared_ptr<RedisValue> debug_object(std::string key);
        std::shared_ptr<RedisValue> debug_segfault();
        std::shared_ptr<RedisValue> flushall(bool async = false);
        std::shared_ptr<RedisValue> flushdb(bool async = false);
        std::shared_ptr<RedisValue> lastsave();
        std::shared_ptr<RedisValue> monitor();
        std::shared_ptr<RedisValue> role();
        std::shared_ptr<RedisValue> save();
        std::shared_ptr<RedisValue> shutdown();
        std::shared_ptr<RedisValue> shutdown(std::string save);
        std::shared_ptr<RedisValue> shutdown(std::string save, std::string nopersist);
        std::shared_ptr<RedisValue> shutdown(std::string save, std::string nopersist, std::string force);
        std::shared_ptr<RedisValue> shutdown(std::string save, std::string nopersist, std::string force, std::string noflush);
        std::shared_ptr<RedisValue> shutdown(std::string save, std::string nopersist, std::string force, std::string noflush, std::string async);
        std::shared_ptr<RedisValue> shutdown(std::string save, std::string nopersist, std::string force, std::string noflush, std::string async, std::string skip_slave_start);
        std::shared_ptr<RedisValue> shutdown(std::string save, std::string nopersist, std::string force, std::string noflush, std::string async, std::string skip_slave_start, std::string no_save);
        
        // stream
        std::shared_ptr<RedisValue> xack(std::string key, std::string group, const std::vector<std::string> &ids);
        std::shared_ptr<RedisValue> xadd(std::string key, std::string id, const std::vector<std::string> &fields);
        std::shared_ptr<RedisValue> xclaim(std::string key, std::string group, std::string consumer, int64_t min_idle_time, const std::vector<std::string> &ids);
        std::shared_ptr<RedisValue> xclaim(std::string key, std::string group, std::string consumer, int64_t min_idle_time, const std::vector<std::string> &ids, int64_t idle_time);
        std::shared_ptr<RedisValue> xclaim(std::string key, std::string group, std::string consumer, int64_t min_idle_time, const std::vector<std::string> &ids, int64_t idle_time, bool just_id);
        std::shared_ptr<RedisValue> xdel(std::string key, const std::vector<std::string> &ids);
        std::shared_ptr<RedisValue> xgroup_create(std::string key, std::string group, std::string id, bool mkstream = false);
        std::shared_ptr<RedisValue> xgroup_setid(std::string key, std::string group, std::string id);
        std::shared_ptr<RedisValue> xgroup_destroy(std::string key, std::string group);
        std::shared_ptr<RedisValue> xgroup_delconsumer(std::string key, std::string group, std::string consumer);
        std::shared_ptr<RedisValue> xinfo_stream(std::string key);
        std::shared_ptr<RedisValue> xinfo_groups(std::string key);
        std::shared_ptr<RedisValue> xinfo_consumers(std::string key, std::string group);
        std::shared_ptr<RedisValue> xlen(std::string key);
        std::shared_ptr<RedisValue> xpending(std::string key, std::string group);
        std::shared_ptr<RedisValue> xpending(std::string key, std::string group, std::string start, std::string end, int count, std::string consumer);
        std::shared_ptr<RedisValue> xrange(std::string key, std::string start, std::string end);
        std::shared_ptr<RedisValue> xrange(std::string key, std::string start, std::string end, int count);
        std::shared_ptr<RedisValue> xread(const std::vector<std::string> &streams);
        std::shared_ptr<RedisValue> xread(const std::vector<std::string> &streams, int count);
        std::shared_ptr<RedisValue> xread(const std::vector<std::string> &streams, int count, bool block);
        std::shared_ptr<RedisValue> xread(const std::vector<std::string> &streams, int count, bool block, int block_time);
        std::shared_ptr<RedisValue> xreadgroup(std::string group, std::string consumer, const std::vector<std::string> &streams);
        std::shared_ptr<RedisValue> xreadgroup(std::string group, std::string consumer, const std::vector<std::string> &streams, int count);
        std::shared_ptr<RedisValue> xreadgroup(std::string group, std::string consumer, const std::vector<std::string> &streams, int count, bool block);
        std::shared_ptr<RedisValue> xreadgroup(std::string group, std::string consumer, const std::vector<std::string> &streams, int count, bool block, int block_time);
        std::shared_ptr<RedisValue> xrevrange(std::string key, std::string end, std::string start);
        std::shared_ptr<RedisValue> xrevrange(std::string key, std::string end, std::string start, int count);
        std::shared_ptr<RedisValue> xtrim(std::string key, int64_t max_len, bool approximate);

    private:
        int connect();
        
    private:
        class Impl;
        std::unique_ptr<Impl> impl_;
    };
}

#endif // __YUAN_REDIS_CLIENT_H__
