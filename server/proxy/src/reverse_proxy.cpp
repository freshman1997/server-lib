#include <cassert>
#include <cstdlib>
#include <ctime>
#include <memory>
#include "logger.h"
#include <algorithm>

#include "base/time.h"

#include "net/connection/connection_factory.h"
#include "net/connection/connection.h"
#include "net/runtime/network_runtime.h"
#include "net/socket/inet_address.h"
#include "net/socket/socket.h"
#include "ops/option.h"
#include "ops/config_manager.h"
#include "reverse_proxy.h"
#include "response_code.h"
#include "nlohmann/json.hpp"
#include "context.h"
#include "request.h"
#include "response.h"
#include "http_server.h"
#include "timer/timer.h"
#include "coroutine/connect_awaitable.h"

namespace yuan::net::http
{
    namespace
    {
        const char *connect_result_text(yuan::coroutine::ConnectResult result)
        {
            using Result = yuan::coroutine::ConnectResult;
            switch (result) {
            case Result::success:
                return "success";
            case Result::invalid_address:
                return "invalid_address";
            case Result::socket_error:
                return "socket_error";
            case Result::connect_failed:
                return "connect_failed";
            case Result::timed_out:
                return "timed_out";
            case Result::connection_error:
                return "connection_error";
            default:
                return "unknown";
            }
        }
    }


    bool PooledConnection::is_expired(uint64_t max_idle_ms) const
    {
        const auto now_ms = base::time::steady_now_ms();
        return !in_use.load(std::memory_order_relaxed) &&
               now_ms > last_used_ms + max_idle_ms;
    }

    void PooledConnection::mark_used()
    {
        in_use.store(true, std::memory_order_relaxed);
        ++ref_count;
        last_used_ms = base::time::steady_now_ms();
    }

    TargetConnectionPool::TargetConnectionPool(const ProxyTarget & target, size_t max_size,
                                               std::chrono::seconds idle_timeout)
        : target_(target), max_size_(max_size), idle_timeout_(idle_timeout)
    {
    }

    TargetConnectionPool::~TargetConnectionPool()
    {
        close_all();
    }

    Connection *TargetConnectionPool::acquire(HttpProxy * proxy, HttpServer * server)
    {
        std::lock_guard<std::mutex> lock(mutex_);

        size_t start = rr_index_.load(std::memory_order_relaxed);
        size_t count = connections_.size();

        for (size_t i = 0; i < count; ++i) {
            size_t idx = (start + i) % count;
            auto &pc = connections_[idx];

            bool expected = false;
            if (!pc.in_use.load(std::memory_order_acquire) &&
                pc.in_use.compare_exchange_strong(expected, true,
                                                  std::memory_order_acq_rel, std::memory_order_relaxed)) {
                pc.mark_used();
                rr_index_.store((idx + 1) % count, std::memory_order_relaxed);
                return pc.conn;
            }
        }

        if (connections_.size() < max_size_) {
            return create_new_connection(proxy, server);
        }

        return nullptr;
    }

    void TargetConnectionPool::release(Connection * conn)
    {
        std::lock_guard<std::mutex> lock(mutex_);

        for (auto &pc : connections_) {
            if (pc.conn == conn && pc.in_use.load(std::memory_order_relaxed)) {
                --pc.ref_count;
                pc.in_use.store(false, std::memory_order_release);
                pc.last_used_ms = base::time::steady_now_ms();
                return;
            }
        }
    }
    void TargetConnectionPool::remove(Connection * conn)
    {
        Connection *to_close = nullptr;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (auto it = connections_.begin(); it != connections_.end(); ++it) {
                if (it->conn == conn) {
                    to_close = it->conn;
                    connections_.erase(it);
                    break;
                }
            }
        }
        if (to_close) {
            to_close->close();
        }
    }

    size_t TargetConnectionPool::cleanup_idle()
    {
        size_t removed = 0;
        const auto now_ms = base::time::steady_now_ms();
        std::vector<Connection *> to_close;

        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (auto it = connections_.begin(); it != connections_.end();) {
                if (!it->in_use.load(std::memory_order_relaxed) &&
                    now_ms > it->last_used_ms + static_cast<uint64_t>(idle_timeout_.count()) * 1000ULL) {
                    if (it->conn) {
                        to_close.push_back(it->conn);
                    }
                    it = connections_.erase(it);
                    ++removed;
                } else {
                    ++it;
                }
            }

            if (!connections_.empty()) {
                rr_index_.store(0, std::memory_order_relaxed);
            }
        }

        for (auto *conn : to_close) {
            conn->close();
        }

        return removed;
    }

    void TargetConnectionPool::close_all()
    {
        std::vector<Connection *> to_close;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (auto &pc : connections_) {
                if (pc.conn) {
                    to_close.push_back(pc.conn);
                }
            }
            connections_.clear();
            rr_index_.store(0, std::memory_order_relaxed);
        }
        for (auto *conn : to_close) {
            conn->close();
        }
    }

    size_t TargetConnectionPool::active_count() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        size_t n = 0;
        for (const auto &pc : connections_) {
            if (pc.in_use.load(std::memory_order_relaxed))
                ++n;
        }
        return n;
    }

    size_t TargetConnectionPool::total_count() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return connections_.size();
    }

    size_t TargetConnectionPool::available_count() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        size_t active = 0;
        for (const auto &pc : connections_) {
            if (pc.in_use.load(std::memory_order_relaxed)) {
                ++active;
            }
        }
        return connections_.size() - active;
    }

    Connection *TargetConnectionPool::create_new_connection(HttpProxy * proxy, HttpServer * server)
    {
        if (!server || !proxy)
            return nullptr;

        auto *runtime = server->runtime();
        if (!runtime)
            return nullptr;

        auto sock = new net::Socket(target_.host.c_str(), target_.port);
        if (!sock->valid()) {
            delete sock;
            return nullptr;
        }

        auto conn = create_stream_connection(sock);
        runtime->register_connection(conn, make_non_owning_handler(proxy));

        PooledConnection pc;
        pc.conn = &*conn;
        pc.created_at_ms = base::time::steady_now_ms();
        pc.mark_used();

        connections_.push_back(std::move(pc));
        return &*conn;
    }

    HttpProxy::HttpProxy()
    {
        rng_.seed(static_cast<unsigned>(base::time::system_now_seconds()));
    }

    HttpProxy::HttpProxy(HttpServer * server)
        : server_(server)
    {
        rng_.seed(static_cast<unsigned>(base::time::system_now_seconds()));
    }

    HttpProxy::~HttpProxy()
    {
        std::vector<Connection *> mapped_connections;
        {
            std::lock_guard<std::mutex> lock(mapping_mutex_);
            for (auto &pair : sc_mapping_) {
                if (pair.first)
                    mapped_connections.push_back(pair.first);
                if (pair.second.client_conn)
                    mapped_connections.push_back(pair.second.client_conn);
            }
        }

        std::vector<std::shared_ptr<TargetConnectionPool> > pool_snapshot;
        {
            std::lock_guard<std::mutex> lock(pools_mutex_);
            for (auto &pair : pools_) {
                if (pair.second) {
                    pool_snapshot.push_back(pair.second);
                }
            }
        }

        for (auto &pool : pool_snapshot) {
            pool->close_all();
        }

        for (auto *conn : mapped_connections) {
            conn->close();
        }

        {
            std::lock_guard<std::mutex> lock(mapping_mutex_);
            sc_mapping_.clear();
            cs_mapping_.clear();
            pending_requests_.clear();
        }
        {
            std::lock_guard<std::mutex> lock(pools_mutex_);
            pools_.clear();
        }
    }

    void HttpProxy::on_connected(const std::shared_ptr<Connection> &conn)
    {
        if (!conn) {
            return;
        }

        (void)conn;
    }

    void HttpProxy::on_error(const std::shared_ptr<Connection> &conn)
    {
        if (!conn) {
            return;
        }

        (void)unmap_and_close_peer(&*conn, false);
    }

    void HttpProxy::on_read(const std::shared_ptr<Connection> &conn)
    {
        if (!conn) {
            return;
        }

        auto &conn_ref = *conn;
        auto *conn_ptr = &conn_ref;
        ServerMapping mapping;
        bool is_server = false;

        {
            std::lock_guard<std::mutex> lock(mapping_mutex_);

            auto server_it = sc_mapping_.find(conn_ptr);
            if (server_it != sc_mapping_.end()) {
                mapping = server_it->second;
                is_server = true;
            } else {
                auto client_it = cs_mapping_.find(conn_ptr);
                if (client_it != cs_mapping_.end() && client_it->second) {
                    forward_data(conn_ptr, client_it->second);
                    client_it->second->flush();
                    return;
                } else {
                    return;
                }
            }
        }

        if (is_server && mapping.client_conn) {
            forward_data(conn_ptr, mapping.client_conn);
            mapping.client_conn->flush();
        }
    }

    void HttpProxy::on_write(const std::shared_ptr<Connection> &conn)
    {
        (void)conn;
    }

    void HttpProxy::on_close(const std::shared_ptr<Connection> &conn)
    {
        if (!conn) {
            return;
        }

        auto &conn_ref = *conn;
        auto *conn_ptr = &conn_ref;

        bool is_server = false;
        {
            std::lock_guard<std::mutex> lock(mapping_mutex_);
            is_server = (sc_mapping_.find(conn_ptr) != sc_mapping_.end());
        }

        const bool unmapped = unmap_and_close_peer(conn_ptr, is_server);
        if (!unmapped) {
            ++stats_.unmapped_close_events;
            LOG_DEBUG_TAG("on_close",
                          "[Proxy] close without mapping peer={} side={}",
                          conn_ptr->get_remote_address().to_address_key(),
                          is_server ? "server" : "client");
        }
        remove_connection_from_pools(conn_ptr);
    }

    bool HttpProxy::load_proxy_config_and_init()
    {
        auto cfgManager = HttpConfigManager::get_instance();
        if (!cfgManager || !cfgManager->good()) {
            LOG_WARN_TAG("load_proxy_config_and_init", "[Proxy] config manager not available");
            return false;
        }

        const auto &proxiesCfg = cfgManager->get_type_array_properties<nlohmann::json>("proxies");
        if (proxiesCfg.empty()) {
            return false;
        }

        assert(server_);
        LOG_INFO_TAG("load_proxy_config_and_init", "[Proxy] loading proxy configs...");

        for (const auto &proxyCfg : proxiesCfg) {
            if (!proxyCfg.is_object()) {
                continue;
            }

            ProxyRoute route;

            if (proxyCfg.contains("root") && proxyCfg["root"].is_string()) {
                route.match_pattern = proxyCfg["root"].get<std::string>();
            } else {
                continue;
            }

            if (proxyCfg.contains("target") && proxyCfg["target"].is_array()) {
                for (const auto &t : proxyCfg["target"]) {
                    if (!t.is_array() || t.size() < 2 || !t[0].is_string() || !t[1].is_number_unsigned()) {
                        continue;
                    }

                    ProxyTarget tgt;
                    tgt.host = t[0].get<std::string>();
                    tgt.port = t[1].get<uint16_t>();
                    if (proxyCfg.contains("weight") && proxyCfg["weight"].is_number()) {
                        tgt.weight = proxyCfg["weight"].get<int>();
                    }
                    route.targets.push_back(tgt);
                }
            }

            if (route.targets.empty()) {
                LOG_WARN_TAG("load_proxy_config_and_init", "[Proxy] route '{}' has no valid targets, skipping", route.match_pattern);
                continue;
            }

            if (proxyCfg.contains("balance") && proxyCfg["balance"].is_string()) {
                const std::string &bs = proxyCfg["balance"].get<std::string>();
                if (bs == "random") {
                    route.balance = ProxyRoute::BalanceStrategy::random;
                } else if (bs == "least_conn") {
                    route.balance = ProxyRoute::BalanceStrategy::least_connections;
                } else if (bs == "weighted_rr") {
                    route.balance = ProxyRoute::BalanceStrategy::weighted_round_robin;
                } else {
                    route.balance = ProxyRoute::BalanceStrategy::round_robin;
                }
            }

            if (proxyCfg.contains("strip_prefix") && proxyCfg["strip_prefix"].is_boolean()) {
                route.strip_prefix = proxyCfg["strip_prefix"].get<bool>();
            }
            if (proxyCfg.contains("rewrite") && proxyCfg["rewrite"].is_string()) {
                route.rewrite_prefix = proxyCfg["rewrite"].get<std::string>();
            }
            if (proxyCfg.contains("connect_timeout") && proxyCfg["connect_timeout"].is_number()) {
                route.connect_timeout_ms = proxyCfg["connect_timeout"].get<int>();
            }
            if (proxyCfg.contains("read_timeout") && proxyCfg["read_timeout"].is_number()) {
                route.read_timeout_ms = proxyCfg["read_timeout"].get<int>();
            }
            if (proxyCfg.contains("write_timeout") && proxyCfg["write_timeout"].is_number()) {
                route.write_timeout_ms = proxyCfg["write_timeout"].get<int>();
            }
            if (proxyCfg.contains("max_retries") && proxyCfg["max_retries"].is_number()) {
                route.max_retries = proxyCfg["max_retries"].get<int>();
            }
            if (proxyCfg.contains("pool_size") && proxyCfg["pool_size"].is_number_unsigned()) {
                route.max_pool_size_per_target = proxyCfg["pool_size"].get<size_t>();
            }
            if (proxyCfg.contains("idle_timeout") && proxyCfg["idle_timeout"].is_number()) {
                route.idle_timeout_seconds = proxyCfg["idle_timeout"].get<size_t>();
            }

            add_route(route);
        }

        return !routes_.empty();
    }

    void HttpProxy::add_route(const ProxyRoute & route)
    {
        std::lock_guard<std::mutex> route_lock(route_mutex_);
        routes_[route.match_pattern] = std::move(route);
        url_trie_.insert(route.match_pattern, true);

        LOG_INFO_TAG("add_route", "[Proxy] register route: pattern='{}', targets={}, balance={}", route.match_pattern, route.targets.size(), static_cast<int>(routes_[route.match_pattern].balance));

        for (const auto &tgt : routes_[route.match_pattern].targets) {
            LOG_INFO_TAG("add_route", "[Proxy] -> {}:{} (weight={})", tgt.host, tgt.port, tgt.weight);
        }
    }

    void HttpProxy::clear_routes()
    {
        std::lock_guard<std::mutex> route_lock(route_mutex_);
        std::lock_guard<std::mutex> lock(rr_mutex_);
        routes_.clear();
        rr_indices_.clear();
        url_trie_.clear();
    }

    std::string HttpProxy::find_proxy_route(const std::string & url) const
    {
        std::lock_guard<std::mutex> route_lock(route_mutex_);
        auto result = url_trie_.find_prefix(url);
        if (!result || !result.is_registered)
            return "";
        return url.substr(0, static_cast<size_t>(result.match_length));
    }

    bool HttpProxy::is_proxy_url(const std::string & url) const
    {
        return !find_proxy_route(url).empty();
    }

    void HttpProxy::handle_websocket_upgrade_by_url(HttpRequest * req, HttpResponse * resp, const std::string & route_key)
    {
        if (!req || !resp || route_key.empty()) {
            if (resp)
                resp->process_error(ResponseCode::bad_gateway);
            return;
        }

        ProxyRoute route;
        {
            std::lock_guard<std::mutex> route_lock(route_mutex_);
            auto routeIt = routes_.find(route_key);
            if (routeIt == routes_.end()) {
                resp->process_error(ResponseCode::bad_gateway);
                return;
            }
            route = routeIt->second;
        }

        ProxyTarget target = select_target(route);
        auto *ctx = req->get_context();
        Connection *clientConn = ctx ? ctx->get_connection() : nullptr;
        if (!clientConn || !server_ || !server_->runtime()) {
            resp->process_error(ResponseCode::internal_server_error);
            ++stats_.failed_requests;
            return;
        }

        bool mapped = false;
        {
            std::lock_guard<std::mutex> lock(mapping_mutex_);
            mapped = (cs_mapping_.find(clientConn) != cs_mapping_.end());
        }
        if (mapped) {
            ++stats_.ws_duplicate_upgrade_skipped;
            LOG_DEBUG_TAG("handle_websocket_upgrade_by_url",
                          "[Proxy][WS] skip duplicate upgrade route='{}' client={}",
                          route_key,
                          clientConn->get_remote_address().to_address_key());
            return;
        }

        auto task = handle_websocket_upgrade_async(req, resp, route_key, route, target, clientConn);
        task.detach();
    }

    void HttpProxy::serve_proxy(HttpRequest * req, HttpResponse * resp)
    {
        if (!req || !resp) {
            return;
        }

        ++stats_.total_requests;

        auto *ctx = req->get_context();
        Connection *clientConn = ctx ? ctx->get_connection() : nullptr;
        if (!clientConn) {
            resp->process_error(ResponseCode::internal_server_error);
            ++stats_.failed_requests;
            return;
        }

        const std::string route_key = find_proxy_route(req->get_raw_url());
        if (route_key.empty()) {
            resp->process_error(ResponseCode::not_found);
            return;
        }

        ProxyRoute route;
        {
            std::lock_guard<std::mutex> route_lock(route_mutex_);
            auto routeIt = routes_.find(route_key);
            if (routeIt == routes_.end()) {
                resp->process_error(ResponseCode::bad_gateway);
                ++stats_.failed_requests;
                return;
            }
            route = routeIt->second;
        }

        ProxyTarget target = select_target(route);
        build_forward_request(req, route, target);

        {
            std::lock_guard<std::mutex> lock(mapping_mutex_);
            auto csIt = cs_mapping_.find(clientConn);
            if (csIt != cs_mapping_.end() && csIt->second) {
                req->pack_and_send(csIt->second);
                csIt->second->flush();
                return;
            }

            auto reqIt = pending_requests_.find(clientConn);
            if (reqIt != pending_requests_.end()) {
                if (reqIt->second >= config::proxy_max_pending) {
                    resp->process_error(ResponseCode::service_unavailable);
                    ++stats_.failed_requests;
                } else {
                    ++reqIt->second;
                }
                return;
            }
        }

        auto task = handle_proxy_async(req, resp, route_key, route, target, clientConn);
        task.detach();
    }

    yuan::coroutine::Task<void> HttpProxy::handle_proxy_async(HttpRequest *req, HttpResponse *resp,
                                                              const std::string &route_key,
                                                              const ProxyRoute &route,
                                                              const ProxyTarget &target,
                                                              Connection *clientConn)
    {
        if (!server_ || !server_->runtime() || !clientConn || !req || !resp) {
            co_return;
        }

        auto pool = get_or_create_pool(target, route);
        if (pool) {
            Connection *pooledConn = pool->acquire(this, server_);
            if (pooledConn) {
                ++stats_.pool_hits;
                LOG_DEBUG_TAG("handle_proxy_async",
                              "[Proxy] pool hit route='{}' target={}:{} active={} total={}",
                              route_key,
                              target.host,
                              target.port,
                              pool->active_count(),
                              pool->total_count());
                map_connections(clientConn->shared_from_this(), pooledConn->shared_from_this(), route_key);
                req->pack_and_send(pooledConn);
                pooledConn->flush();
                co_return;
            }
        }

        ++stats_.pool_misses;
        LOG_DEBUG_TAG("handle_proxy_async",
                      "[Proxy] pool miss route='{}' target={}:{} timeout_ms={}",
                      route_key,
                      target.host,
                      target.port,
                      route.connect_timeout_ms);

        const uint64_t dial_start_ms = base::time::steady_now_ms();

        auto connect_result = co_await yuan::coroutine::async_connect(
            static_cast<yuan::coroutine::RuntimeView>(server_->runtime()->runtime_view()),
            target.host,
            target.port,
            route.connect_timeout_ms > 0 ? static_cast<uint32_t>(route.connect_timeout_ms) : 0U);

        const uint64_t dial_elapsed_ms = base::time::steady_now_ms() - dial_start_ms;

        if (connect_result.result != yuan::coroutine::ConnectResult::success || !connect_result.connection) {
            LOG_WARN_TAG("handle_proxy_async",
                         "[Proxy] upstream dial failed route='{}' target={}:{} result={} elapsed_ms={} timeout_ms={}",
                         route_key,
                         target.host,
                         target.port,
                         connect_result_text(connect_result.result),
                         dial_elapsed_ms,
                         route.connect_timeout_ms);
            resp->process_error(connect_result.result == yuan::coroutine::ConnectResult::timed_out
                                    ? ResponseCode::gateway_timeout
                                    : ResponseCode::bad_gateway);
            ++stats_.failed_requests;
            co_return;
        }

        {
            std::lock_guard<std::mutex> lock(mapping_mutex_);
            if (cs_mapping_.find(clientConn) != cs_mapping_.end()) {
                ++stats_.ws_stale_upgrade_skipped;
                LOG_DEBUG_TAG("handle_websocket_upgrade_async",
                              "[Proxy][WS] skip stale upgrade after connect route='{}' target={}:{}",
                              route_key,
                              target.host,
                              target.port);
                co_return;
            }
        }

        LOG_DEBUG_TAG("handle_proxy_async",
                      "[Proxy] upstream dial success route='{}' target={}:{} elapsed_ms={}",
                      route_key,
                      target.host,
                      target.port,
                      dial_elapsed_ms);

        auto remote_owner = connect_result.connection;
        Connection *remoteConn = &*remote_owner;
        server_->runtime()->register_connection(remote_owner, make_non_owning_handler(this));

        ++stats_.active_connections;
        map_connections(clientConn->shared_from_this(), remote_owner, route_key);
        req->pack_and_send(remoteConn);
        remoteConn->flush();
        co_return;
    }

    void HttpProxy::on_client_close(const std::shared_ptr<Connection> &conn)
    {
        if (!conn) {
            return;
        }

        auto *conn_ptr = &*conn;
        (void)unmap_and_close_peer(conn_ptr, true);
        std::lock_guard<std::mutex> lock(mapping_mutex_);
        pending_requests_.erase(conn_ptr);
    }

    ProxyTarget HttpProxy::select_target(const ProxyRoute & route)
    {
        if (route.targets.empty())
            return ProxyTarget{};

        switch (route.balance) {
        case ProxyRoute::BalanceStrategy::round_robin: {
            std::lock_guard<std::mutex> lock(rr_mutex_);
            auto &idx = rr_indices_[route.match_pattern];
            size_t i = idx % route.targets.size();
            idx++;
            return route.targets[i];
        }
        case ProxyRoute::BalanceStrategy::random: {
            std::lock_guard<std::mutex> lock(rr_mutex_);
            std::uniform_int_distribution<size_t> dist(0, route.targets.size() - 1);
            return route.targets[dist(rng_)];
        }
        case ProxyRoute::BalanceStrategy::least_connections:
            return select_least_connections(route);
        case ProxyRoute::BalanceStrategy::weighted_round_robin:
            return select_weighted_random(route.targets);
        default:
            return route.targets[0];
        }
    }

    ProxyTarget HttpProxy::select_weighted_random(const std::vector<ProxyTarget> & targets)
    {
        int total_weight = 0;
        for (const auto &t : targets)
            total_weight += (std::max)(1, t.weight);

        std::uniform_int_distribution<int> dist(1, total_weight);
        int rand_val = dist(rng_);

        int cumulative = 0;
        for (const auto &t : targets) {
            cumulative += (std::max)(1, t.weight);
            if (rand_val <= cumulative)
                return t;
        }
        return targets.back();
    }

    ProxyTarget HttpProxy::select_least_connections(const ProxyRoute & route)
    {
        ProxyTarget best = route.targets[0];
        size_t min_active = SIZE_MAX;

        for (const auto &target : route.targets) {
            std::string pool_id = target.host + ":" + std::to_string(target.port);

            std::lock_guard<std::mutex> lock(pools_mutex_);
            auto it = pools_.find(pool_id);
            if (it != pools_.end() && it->second) {
                size_t active = it->second->active_count();
                if (active < min_active) {
                    min_active = active;
                    best = target;
                }
            } else {
                best = target;
                min_active = 0;
                break;
            }
        }

        return best;
    }

    void HttpProxy::build_forward_request(HttpRequest * orig_req, const ProxyRoute & route,
                                            const ProxyTarget & target, bool is_websocket)
    {
        orig_req->add_header("Host", target.host + ":" + std::to_string(target.port));

        auto *xff = orig_req->get_header("x-forwarded-for");
        if (xff && orig_req->get_context() && orig_req->get_context()->get_connection()) {
            const auto &addr = orig_req->get_context()->get_connection()->get_remote_address();
            std::string new_xff = xff->c_str();
            new_xff += ", ";
            new_xff += addr.to_address_key();
            orig_req->add_header("X-Forwarded-For", std::move(new_xff));
        } else if (orig_req->get_context() && orig_req->get_context()->get_connection()) {
            const auto &addr = orig_req->get_context()->get_connection()->get_remote_address();
            orig_req->add_header("X-Forwarded-For", addr.to_address_key());
        }

        if (orig_req->get_context() && orig_req->get_context()->get_connection()) {
            const auto &addr = orig_req->get_context()->get_connection()->get_remote_address();
            orig_req->add_header("X-Real-IP", addr.to_address_key());
        }

        orig_req->add_header("X-Forwarded-Proto", "http");

        if (!is_websocket) {
            orig_req->remove_header("connection");
            orig_req->remove_header("upgrade");
        }
        orig_req->remove_header("keep-alive");
        orig_req->remove_header("transfer-encoding");
        orig_req->remove_header("te");
        orig_req->remove_header("trailers");

        if (route.strip_prefix) {
            auto path = std::string(orig_req->get_path());
            auto query = std::string(orig_req->get_query_string());
            if (path.size() >= route.match_pattern.size()
                && path.compare(0, route.match_pattern.size(), route.match_pattern) == 0) {
                path.erase(0, route.match_pattern.size());
            }
            if (!route.rewrite_prefix.empty()) {
                path = route.rewrite_prefix + path;
            }
            std::string new_url = path;
            if (!query.empty()) {
                new_url += "?";
                new_url += query;
            }
            orig_req->set_raw_url(std::move(new_url));
        }
    }

    yuan::coroutine::Task<void> HttpProxy::handle_websocket_upgrade_async(HttpRequest *req,
                                                                           HttpResponse *resp,
                                                                           const std::string &route_key,
                                                                           const ProxyRoute &route,
                                                                           const ProxyTarget &target,
                                                                           Connection *clientConn)
    {
        if (!req || !resp || !clientConn || !server_ || !server_->runtime()) {
            co_return;
        }

        LOG_DEBUG_TAG("handle_websocket_upgrade_async",
                      "[Proxy][WS] dialing upstream route='{}' target={}:{} timeout_ms={}",
                      route_key,
                      target.host,
                      target.port,
                      route.connect_timeout_ms);

        const uint64_t dial_start_ms = base::time::steady_now_ms();

        auto connect_result = co_await yuan::coroutine::async_connect(
            static_cast<yuan::coroutine::RuntimeView>(server_->runtime()->runtime_view()),
            target.host,
            target.port,
            route.connect_timeout_ms > 0 ? static_cast<uint32_t>(route.connect_timeout_ms) : 0U);

        const uint64_t dial_elapsed_ms = base::time::steady_now_ms() - dial_start_ms;

        if (connect_result.result != yuan::coroutine::ConnectResult::success || !connect_result.connection) {
            LOG_WARN_TAG("handle_websocket_upgrade_async",
                         "[Proxy][WS] upstream dial failed route='{}' target={}:{} result={} elapsed_ms={} timeout_ms={}",
                         route_key,
                         target.host,
                         target.port,
                         connect_result_text(connect_result.result),
                         dial_elapsed_ms,
                         route.connect_timeout_ms);
            resp->process_error(connect_result.result == yuan::coroutine::ConnectResult::timed_out
                                    ? ResponseCode::gateway_timeout
                                    : ResponseCode::bad_gateway);
            ++stats_.failed_requests;
            co_return;
        }

        LOG_DEBUG_TAG("handle_websocket_upgrade_async",
                      "[Proxy][WS] upstream dial success route='{}' target={}:{} elapsed_ms={}",
                      route_key,
                      target.host,
                      target.port,
                      dial_elapsed_ms);

        auto remote_owner = connect_result.connection;
        Connection *remoteConn = &*remote_owner;
        server_->runtime()->register_connection(remote_owner, make_non_owning_handler(this));

        ++stats_.active_connections;
        map_connections(clientConn->shared_from_this(), remote_owner, route_key);

        req->pack_and_send(remoteConn);
        remoteConn->flush();
        co_return;
    }

    void HttpProxy::map_connections(Connection * clientConn, Connection * serverConn, const std::string & routeKey)
    {
        std::lock_guard<std::mutex> lock(mapping_mutex_);
        cs_mapping_[clientConn] = serverConn;

            ServerMapping sm;
            sm.client_conn = clientConn;
            sm.route_key = routeKey;
        sc_mapping_[serverConn] = std::move(sm);
    }

    void HttpProxy::map_connections(const std::shared_ptr<Connection> &clientConn,
                                    const std::shared_ptr<Connection> &serverConn,
                                    const std::string &routeKey)
    {
        if (!clientConn || !serverConn) {
            return;
        }
        map_connections(&*clientConn, &*serverConn, routeKey);
    }

    bool HttpProxy::unmap_and_close_peer(Connection * conn, bool is_client)
    {
        Connection *peer_to_close = nullptr;
        bool unmapped = false;
        {
            std::lock_guard<std::mutex> lock(mapping_mutex_);

            if (is_client) {
                auto it = cs_mapping_.find(conn);
                if (it != cs_mapping_.end()) {
                    Connection *serverConn = it->second;
                    cs_mapping_.erase(it);
                    unmapped = true;

                    if (serverConn) {
                        auto sit = sc_mapping_.find(serverConn);
                        if (sit != sc_mapping_.end()) {
                            sc_mapping_.erase(sit);
                        }
                        peer_to_close = serverConn;
                    }
                }
            } else {
                auto it = sc_mapping_.find(conn);
                if (it != sc_mapping_.end()) {
                    Connection *clientConn = it->second.client_conn;
                    sc_mapping_.erase(it);
                    unmapped = true;

                    if (clientConn) {
                        auto cit = cs_mapping_.find(clientConn);
                        if (cit != cs_mapping_.end()) {
                            cs_mapping_.erase(cit);
                        }
                        peer_to_close = clientConn;
                    }
                }
            }
        }

        if (peer_to_close) {
            peer_to_close->close();
        }

        return unmapped;
    }

    void HttpProxy::forward_data(Connection * src, Connection * dst)
    {
        if (!src || !dst)
            return;

        const auto input = src->take_input_byte_buffer();
        if (input.empty())
            return;
        dst->write(input);
    }

    bool HttpProxy::handle_websocket_upgrade(HttpRequest * req, HttpResponse * resp,
                                                const ProxyRoute & route, const ProxyTarget & target)
    {
        auto *upgrade = req->get_header("upgrade");
        if (!upgrade)
            return false;

        std::string upgrade_lower = *upgrade;
        std::transform(upgrade_lower.begin(), upgrade_lower.end(), upgrade_lower.begin(), ::tolower);
        if (upgrade_lower != "websocket")
            return false;

        build_forward_request(req, route, target, true);

        return true;
    }

    void HttpProxy::remove_connection_from_pools(Connection * conn)
    {
        std::vector<std::shared_ptr<TargetConnectionPool> > pool_snapshot;
        {
            std::lock_guard<std::mutex> lock(pools_mutex_);
            pool_snapshot.reserve(pools_.size());
            for (auto &pair : pools_) {
                if (pair.second) {
                    pool_snapshot.push_back(pair.second);
                }
            }
        }

        for (auto &pool : pool_snapshot) {
            pool->remove(conn);
        }
    }

    void HttpProxy::cleanup_idle_connections()
    {
        std::vector<std::shared_ptr<TargetConnectionPool> > pool_snapshot;
        {
            std::lock_guard<std::mutex> lock(pools_mutex_);
            pool_snapshot.reserve(pools_.size());
            for (auto &pair : pools_) {
                if (pair.second)
                    pool_snapshot.push_back(pair.second);
            }
        }

        size_t total_cleaned = 0;
        for (auto &pool : pool_snapshot) {
            if (pool) {
                total_cleaned += pool->cleanup_idle();
            }
        }

        if (total_cleaned > 0) {
            LOG_INFO_TAG("cleanup_idle_connections", "[Proxy] cleaned up {} idle connections from pool", total_cleaned);
        }
    }

    std::shared_ptr<TargetConnectionPool> HttpProxy::get_or_create_pool(const ProxyTarget & target,
                                                                        const ProxyRoute & route)
    {
        std::string pool_id = target.host + ":" + std::to_string(target.port);

        {
            std::lock_guard<std::mutex> lock(pools_mutex_);
            auto it = pools_.find(pool_id);
            if (it != pools_.end() && it->second) {
                return it->second;
            }
        }

        auto pool = std::make_shared<TargetConnectionPool>(
            target,
            route.max_pool_size_per_target,
            std::chrono::seconds(route.idle_timeout_seconds));

        {
            std::lock_guard<std::mutex> lock(pools_mutex_);
            auto it = pools_.find(pool_id);
            if (it != pools_.end() && it->second) {
                return it->second;
            }
            pools_[pool_id] = pool;
        }

        return pool;
    }

    } // namespace yuan::net::http
