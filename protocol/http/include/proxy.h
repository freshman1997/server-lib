#ifndef __NET_HTTP_PROXY_H__
#define __NET_HTTP_PROXY_H__

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

#include "base/utils/compressed_trie.h"
#include "common.h"
#include "middleware.h"
#include "net/connection/connection.h"
#include "net/handler/connection_handler.h"
#include "net/socket/inet_address.h"
#include "timer/timer.h"

namespace yuan::timer
{
    class Timer;
}

namespace yuan::net::http
{
    class HttpServer;
    class HttpRequest;
    class HttpResponse;
    class RemoteConnectTask;
    class HttpProxy;

    struct ProxyTarget
    {
        std::string host;
        uint16_t port = 0;
        int weight = 1;
        std::string prefix_rewrite;

        InetAddress address() const
        {
            return InetAddress(host, port);
        }
    };

    struct ProxyRoute
    {
        std::string match_pattern;
        std::vector<ProxyTarget> targets;

        enum class BalanceStrategy : uint8_t {
            round_robin,
            random,
            least_connections,
            weighted_round_robin
        } balance = BalanceStrategy::round_robin;

        bool strip_prefix = true;
        std::string rewrite_prefix;
        int connect_timeout_ms = 5000;
        int read_timeout_ms = 30000;
        int write_timeout_ms = 10000;
        int max_retries = 2;
        size_t max_pool_size_per_target = 8;
        size_t idle_timeout_seconds = 60;
    };

    class RemoteConnectTask
    {
    public:
        using Ptr = std::shared_ptr<RemoteConnectTask>;

        RemoteConnectTask(
            Connection *clientConn,
            HttpRequest *req,
            HttpResponse *resp,
            HttpProxy *proxy,
            const std::string &routeKey,
            const ProxyTarget &target);

        Connection *client_conn_ = nullptr;
        HttpRequest *req_ = nullptr;
        HttpResponse *resp_ = nullptr;
        HttpProxy *proxy_ = nullptr;
        std::string route_key_;
        ProxyTarget target_;
        int retry_count_ = 0;
    };

    struct PooledConnection
    {
        Connection *conn = nullptr;
        std::atomic<bool> in_use{ false };
        std::atomic<int> ref_count{ 0 };
        uint64_t last_used_ms = 0;
        uint64_t created_at_ms = 0;

        PooledConnection() = default;
        PooledConnection(PooledConnection &&other) noexcept
            : conn(other.conn),
              in_use(other.in_use.load(std::memory_order_relaxed)),
              ref_count(other.ref_count.load(std::memory_order_relaxed)),
              last_used_ms(other.last_used_ms),
              created_at_ms(other.created_at_ms)
        {
            other.conn = nullptr;
        }

        PooledConnection &operator=(PooledConnection &&other) noexcept
        {
            if (this != &other) {
                conn = other.conn;
                in_use.store(other.in_use.load(std::memory_order_relaxed), std::memory_order_relaxed);
                ref_count.store(other.ref_count.load(std::memory_order_relaxed), std::memory_order_relaxed);
                last_used_ms = other.last_used_ms;
                created_at_ms = other.created_at_ms;
                other.conn = nullptr;
            }
            return *this;
        }

        PooledConnection(const PooledConnection &) = delete;
        PooledConnection &operator=(const PooledConnection &) = delete;

        bool is_expired(uint64_t max_idle_ms) const;
        void mark_used();
    };

    class TargetConnectionPool
    {
    public:
        explicit TargetConnectionPool(
            const ProxyTarget &target,
            size_t max_size = 8,
            std::chrono::seconds idle_timeout = std::chrono::seconds(60));

        ~TargetConnectionPool();

        Connection *acquire(HttpProxy *proxy, HttpServer *server);
        void release(Connection *conn);
        void remove(Connection *conn);
        size_t cleanup_idle();
        void close_all();

        size_t active_count() const;
        size_t total_count() const;
        size_t available_count() const;

        const ProxyTarget &target() const
        {
            return target_;
        }

    private:
        Connection *create_new_connection(HttpProxy *proxy, HttpServer *server);

    private:
        ProxyTarget target_;
        size_t max_size_;
        std::chrono::seconds idle_timeout_;
        mutable std::mutex mutex_;
        std::vector<PooledConnection> connections_;
        std::atomic<size_t> rr_index_{ 0 };
    };

    class HttpProxy : public ConnectionHandler, public std::enable_shared_from_this<HttpProxy>
    {
    public:
        struct Stats
        {
            std::atomic<uint64_t> total_requests{ 0 };
            std::atomic<uint64_t> active_connections{ 0 };
            std::atomic<uint64_t> failed_requests{ 0 };
            std::atomic<uint64_t> pool_hits{ 0 };
            std::atomic<uint64_t> pool_misses{ 0 };
        };

        HttpProxy();
        explicit HttpProxy(HttpServer *server);
        ~HttpProxy();

        HttpProxy(const HttpProxy &) = delete;
        HttpProxy &operator=(const HttpProxy &) = delete;

        void on_connected(const std::shared_ptr<Connection> &conn) override;
        void on_error(const std::shared_ptr<Connection> &conn) override;
        void on_read(const std::shared_ptr<Connection> &conn) override;
        void on_write(const std::shared_ptr<Connection> &conn) override;
        void on_close(const std::shared_ptr<Connection> &conn) override;

        bool load_proxy_config_and_init();
        void add_route(const ProxyRoute &route);

        void set_server(HttpServer *server)
        {
            server_ = server;
        }
        HttpServer *get_server() const
        {
            return server_;
        }

        std::string find_proxy_route(const std::string &url) const;
        bool is_proxy_url(const std::string &url) const;

        void serve_proxy(HttpRequest *req, HttpResponse *resp);
        void handle_websocket_upgrade_by_url(HttpRequest *req, HttpResponse *resp, const std::string &route_key);
        void on_client_close(const std::shared_ptr<Connection> &conn);

        void on_connection_established(RemoteConnectTask::Ptr task, Connection *remoteConn);
        void on_connection_timeout(RemoteConnectTask::Ptr task);
        void check_response_timer(Connection *conn);

        void cleanup_idle_connections();
        std::shared_ptr<TargetConnectionPool> get_or_create_pool(const ProxyTarget &target, const ProxyRoute &route);

        const Stats &stats() const
        {
            return stats_;
        }

        const std::unordered_map<std::string, ProxyRoute> &get_routes() const
        {
            return routes_;
        }

        ProxyTarget select_target_public(const ProxyRoute &route)
        {
            return select_target(route);
        }

    private:
        ProxyTarget select_target(const ProxyRoute &route);
        ProxyTarget select_weighted_random(const std::vector<ProxyTarget> &targets);
        ProxyTarget select_least_connections(const ProxyRoute &route);

        void build_forward_request(HttpRequest *orig_req, const ProxyRoute &route, const ProxyTarget &target, bool is_websocket = false);
        Connection *create_remote_connection(const ProxyTarget &target, int timeout_ms);
        void map_connections(Connection *clientConn, Connection *serverConn, const std::string &routeKey);
        void map_connections(const std::shared_ptr<Connection> &clientConn, const std::shared_ptr<Connection> &serverConn, const std::string &routeKey);
        void unmap_and_close_peer(Connection *conn, bool is_client);
        void forward_data(Connection *src, Connection *dst);

        bool handle_websocket_upgrade(HttpRequest *req, HttpResponse *resp, const ProxyRoute &route, const ProxyTarget &target);
        timer::Timer *take_timer(Connection *conn);
        void bind_timer(Connection *conn, timer::Timer *timer);
        void remove_connection_from_pools(Connection *conn);

    private:
        HttpServer *server_ = nullptr;
        std::unordered_map<std::string, ProxyRoute> routes_;
        base::CompressTrie url_trie_;

        struct ServerMapping
        {
            Connection *client_conn = nullptr;
            std::string route_key;
        };

        std::unordered_map<Connection *, ServerMapping> sc_mapping_;
        std::unordered_map<Connection *, Connection *> cs_mapping_;
        mutable std::mutex mapping_mutex_;

        std::unordered_map<std::string, std::shared_ptr<TargetConnectionPool> > pools_;
        mutable std::mutex pools_mutex_;

        std::unordered_map<Connection *, RemoteConnectTask::Ptr> pending_tasks_;
        std::unordered_map<Connection *, RemoteConnectTask::Ptr> pending_server_tasks_;

        std::unordered_map<Connection *, timer::Timer *> conn_timers_;
        mutable std::mutex timer_mutex_;

        std::unordered_map<Connection *, int> pending_requests_;
        mutable std::mt19937 rng_{ std::random_device{}() };
        mutable std::mutex rr_mutex_;
        std::unordered_map<std::string, size_t> rr_indices_;
        Stats stats_;
    };
}

#endif
