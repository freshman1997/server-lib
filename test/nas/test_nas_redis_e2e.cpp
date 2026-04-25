#include "nas/nas.h"

#ifdef _WIN32
#include <Windows.h>
#endif

#include <cstdlib>
#include <algorithm>
#include <filesystem>
#include <iostream>
#include <string>

namespace
{
    int failed = 0;

    void check(bool cond, const char *msg)
    {
        if (!cond) {
            ++failed;
            std::cerr << "[FAIL] " << msg << '\n';
        }
    }

    int env_int(const char *name, int fallback)
    {
        const char *value = std::getenv(name);
        return value ? std::atoi(value) : fallback;
    }

    std::string env_string(const char *name, const std::string &fallback)
    {
        const char *value = std::getenv(name);
        return value ? std::string(value) : fallback;
    }
}

int main()
{
#ifdef _WIN32
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        std::cerr << "WSAStartup failed\n";
        return 1;
    }
#endif

#if !YUAN_NAS_HAS_REDIS
    return 2;
#else
    namespace nas = yuan::server::nas;

    nas::NasRedisConfig cfg;
    cfg.host = env_string("REDIS_HOST", "127.0.0.1");
    cfg.port = env_int("REDIS_PORT", 6379);
    cfg.db = env_int("REDIS_DB", 1);
    cfg.password = env_string("REDIS_PASSWORD", "");
    cfg.key_prefix = env_string("NAS_REDIS_PREFIX", "yuan:nas:e2e:");

    nas::NasRedisMetadataStore store(cfg);
    if (!store.init()) {
        std::cerr << "NAS Redis e2e skipped: no Redis at " << cfg.host << ":" << cfg.port << '\n';
#ifdef _WIN32
        WSACleanup();
#endif
        return 2;
    }

    nas::NasUser user;
    user.id = "u-e2e";
    user.username = "alice-e2e";
    user.password_hash = "plain-for-test";
    user.enabled = true;
    user.admin = true;
    check(store.upsert_user(user), "upsert user should succeed");

    auto loaded_user = store.find_user_by_name(user.username);
    check(loaded_user && loaded_user->id == user.id && loaded_user->admin, "user should roundtrip by name");
    auto users = store.list_users();
    check(std::any_of(users.begin(), users.end(), [&](const nas::NasUser &item) {
              return item.id == user.id && item.username == user.username;
          }),
          "list users should include upserted user");

    nas::NasShare share;
    share.id = "s-e2e";
    share.name = "public-e2e";
    share.root_path = "/tmp/yuan-nas-e2e";
    share.default_permissions = nas::NasPermission::read;
    share.subject_permissions[user.id] = nas::NasPermission::read | nas::NasPermission::write;
    check(store.upsert_share(share), "upsert share should succeed");

    auto loaded_share = store.find_share_by_name(share.name);
    check(loaded_share && loaded_share->id == share.id && loaded_share->name == share.name,
          "share should roundtrip by name");
    check(nas::has_permission(store.permissions_for(share.id, user.id), nas::NasPermission::write),
          "ACL should roundtrip");

    check(store.set_dead_property(share.id, "/docs/a.txt", "displayname", "A File"), "dead property set should succeed");
    auto props = store.dead_properties(share.id, "/docs/a.txt");
    check(props.contains("displayname") && props["displayname"] == "A File", "dead property should roundtrip");
    check(store.remove_dead_property(share.id, "/docs/a.txt", "displayname"), "dead property remove should succeed");

    nas::NasAuditEvent event;
    event.timestamp_unix_ms = 4102444800000ll;
    event.actor = user.username;
    event.action = "test.audit";
    event.target = share.name;
    event.detail = "redis";
    check(store.append_audit_event(event), "audit event append should succeed");
    auto audit = store.list_audit_events(10);
    check(!audit.empty() && audit.front().action == event.action && audit.front().target == event.target,
          "audit event should roundtrip");

    nas::NasAdminSession session;
    session.id = "alice-e2e:127.0.0.1";
    session.username = user.username;
    session.remote_addr = "127.0.0.1";
    session.user_agent = "test";
    session.last_path = "/nas/admin/users";
    session.created_at_unix_ms = 4102444800000ll;
    session.last_seen_unix_ms = 4102444801000ll;
    session.request_count = 2;
    check(store.upsert_admin_session(session), "admin session upsert should succeed");
    auto sessions = store.list_admin_sessions(10);
    check(std::any_of(sessions.begin(), sessions.end(), [&](const nas::NasAdminSession &item) {
              return item.id == session.id && item.username == session.username &&
                     item.request_count == session.request_count;
          }),
          "admin session should roundtrip");

    const auto root = std::filesystem::temp_directory_path() / "yuan_nas_redis_e2e_share";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root, ec);
    share.root_path = root.string();
    auto shares = std::make_shared<nas::NasShareManager>(std::vector<nas::NasShare>{ share });
    auto backend = std::make_shared<nas::NasWebDavBackend>(
        shares,
        std::make_shared<nas::NasRedisMetadataStore>(cfg),
        "/dav");
    backend->write("/public-e2e/docs/b.txt", "hello", true);
    check(backend->set_properties("/public-e2e/docs/b.txt", { { "DAV:", "nas-tag", "redis" } }).ok,
          "NAS WebDAV backend should persist dead properties through Redis");
    auto dav_props = backend->dead_properties("/public-e2e/docs/b.txt");
    bool found_prop = false;
    for (const auto &prop : dav_props) {
        found_prop = found_prop || (prop.name == "nas-tag" && prop.value == "redis");
    }
    check(found_prop, "NAS WebDAV backend should read dead properties through Redis");
    std::filesystem::remove_all(root, ec);

    nas::NasWebDavLockRecord lock;
    lock.token = "opaquelocktoken:e2e";
    lock.share_id = share.id;
    lock.path = "/docs";
    lock.scope = "exclusive";
    lock.depth = "infinity";
    lock.owner = "alice";
    lock.expires_at_unix_ms = 4102444800000ll;
    check(store.upsert_webdav_lock(lock), "webdav lock upsert should succeed");
    auto loaded_lock = store.find_webdav_lock(lock.token);
    check(loaded_lock && loaded_lock->owner == "alice", "webdav lock should roundtrip by token");
    auto locks = store.list_webdav_locks(share.id, "/docs/b.txt");
    check(!locks.empty() && locks.front().token == lock.token, "webdav depth lock should cover child path");
    check(store.remove_webdav_lock(lock.token), "webdav lock remove should succeed");

    auto redis_store = std::make_shared<nas::NasRedisMetadataStore>(cfg);
    check(redis_store->init(), "redis store for lock manager should initialize");
    auto redis_shares = std::make_shared<nas::NasShareManager>(std::vector<nas::NasShare>{ share });
    nas::NasRedisWebDavLockManager lock_manager(redis_shares, redis_store, "/dav");
    auto created_lock = lock_manager.create("/public-e2e/docs", yuan::net::webdav::LockScope::exclusive,
                                            yuan::net::webdav::Depth::infinity, "alice",
                                            std::chrono::seconds(3600));
    check(!created_lock.token.empty(), "redis lock manager should create token");
    check(!lock_manager.allows("/public-e2e/docs/b.txt", ""), "redis lock should block child write without token");
    check(lock_manager.allows("/public-e2e/docs/b.txt", "<" + created_lock.token + ">"),
          "redis lock should allow matching token");
    check(lock_manager.unlock("<" + created_lock.token + ">"), "redis lock manager should unlock token");

    if (failed != 0) {
        std::cerr << "nas redis e2e failed=" << failed << '\n';
#ifdef _WIN32
        WSACleanup();
#endif
        return 1;
    }

    std::cout << "nas redis e2e passed\n";
#ifdef _WIN32
    WSACleanup();
#endif
    return 0;
#endif
}
