#include <cassert>
#include <cstdlib>
#include <ctime>
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
#include "proxy.h"
#include "response_code.h"
#include "nlohmann/json.hpp"
#include "context.h"
#include "request.h"
#include "response.h"
#include "http_server.h"
#include "timer/timer.h"

namespace yuan::net::http
{

    RemoteConnectTask::RemoteConnectTask(Connection * clientConn, HttpRequest * req, HttpResponse * resp,
                                         HttpProxy * proxy, const std::string & routeKey, const ProxyTarget & target)
        : client_conn_(clientConn), req_(req), resp_(resp), proxy_(proxy),
          route_key_(routeKey), target_(target)
    {
    }

    // ============================================================
    // PooledConnection 辅助方法
    // ============================================================

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

    // ============================================================
    // TargetConnectionPool 实现
    // ============================================================

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

        // 1. 尝试从池中找一个可用的空闲连接（轮询策略）
        size_t start = rr_index_.load(std::memory_order_relaxed);
        size_t count = connections_.size();

        for (size_t i = 0; i < count; ++i) {
            size_t idx = (start + i) % count;
            auto &pc = connections_[idx];

            bool expected = false;
            if (!pc.in_use.load(std::memory_order_acquire) &&
                pc.in_use.compare_exchange_strong(expected, true,
                                                  std::memory_order_acq_rel, std::memory_order_relaxed)) {
                // 成功获取
                pc.mark_used();
                rr_index_.store((idx + 1) % count, std::memory_order_relaxed);
                return pc.conn;
            }
        }

        // 2. 池中没有可用连接，如果未达上限则新建一个连接
        if (connections_.size() < max_size_) {
            return create_new_connection(proxy, server);
        }

        // 3. 池已满，返回nullptr让调用者决定处理方式
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

        // 未找到，说明可能已被移除，直接关闭
        // 这种情况不应该发生，但做防御性处理
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
            // 重置轮询索引
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

        Connection *conn = create_stream_connection(sock);
        runtime->register_connection(conn, proxy);

        PooledConnection pc;
        pc.conn = conn;
        pc.created_at_ms = base::time::steady_now_ms();
        pc.mark_used(); // 新创建的连接立即标记为使用中

        connections_.push_back(std::move(pc));
        return conn;
    }

    // ============================================================
    // HttpProxy 核心实现
    // ============================================================

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

        std::vector<timer::Timer *> timers;
        {
            std::lock_guard<std::mutex> lock(timer_mutex_);
            for (auto &pair : conn_timers_) {
                if (pair.second) {
                    timers.push_back(pair.second);
                }
            }
        }

        for (auto *timer : timers) {
            timer->cancel();
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
            pending_tasks_.clear();
            pending_server_tasks_.clear();
            pending_requests_.clear();
        }
        {
            std::lock_guard<std::mutex> lock(pools_mutex_);
            pools_.clear();
        }
        {
            std::lock_guard<std::mutex> lock(timer_mutex_);
            conn_timers_.clear();
        }
    }

    // ------------------------------------------------------------
    // ConnectionHandler 接口实现
    // ------------------------------------------------------------

    void HttpProxy::on_connected(Connection * conn)
    {
        // 检查是否有待完成的远程连接任务
        RemoteConnectTask::Ptr task;
        {
            std::lock_guard<std::mutex> lock(mapping_mutex_);
            auto it = pending_server_tasks_.find(conn);
            if (it != pending_server_tasks_.end()) {
                task = it->second;
                pending_server_tasks_.erase(it);
            }
        }

        if (task) {
            on_connection_established(task, conn);
        }
    }

    void HttpProxy::on_error(Connection * conn)
    {
        // 连接出错时清理相关资源   // 可能是远程连接断开、网络错误等
        unmap_and_close_peer(conn, false); // 假设是服务端连接出错
    }

    void HttpProxy::on_read(Connection * conn)
    {
        // 确定这是服务端连接还是客户端连接，然后转发数据
        ServerMapping mapping;
        bool is_server = false;

        {
            std::lock_guard<std::mutex> lock(mapping_mutex_);

            auto server_it = sc_mapping_.find(conn);
            if (server_it != sc_mapping_.end()) {
                mapping = server_it->second;
                is_server = true;
            } else {
                // 这是客户端连接，查找对应的服务端连接
                auto client_it = cs_mapping_.find(conn);
                if (client_it != cs_mapping_.end() && client_it->second) {
                    forward_data(conn, client_it->second);
                    client_it->second->flush();
                    return;
                } else {
                    // 无映射关系，可能是已关闭的客户端连接
                    return;
                }
            }
        }

        // 服务端有数据 -> 转发给客户端
        if (is_server && mapping.client_conn) {
            forward_data(conn, mapping.client_conn);
            mapping.client_conn->flush();
        }
    }

    void HttpProxy::on_write(Connection * conn)
    {
        // 通常不需要特殊处理，数据转发由on_read驱动
    }

    void HttpProxy::on_close(Connection * conn)
    {
        // 远程连接关闭了，需要：
        // 1. 取消关联的超时定时器
        // 2. 清理连接映射
        // 3. 从连接池中移除
        // 4. 通知对端关闭

        // 取消定时器
        timer::Timer *timer_to_cancel = take_timer(conn);
        if (timer_to_cancel) {
            timer_to_cancel->cancel();
        }
        // 判断是服务端还是客户端连接并分别处理
        bool is_server = false;
        {
            std::lock_guard<std::mutex> lock(mapping_mutex_);
            is_server = (sc_mapping_.find(conn) != sc_mapping_.end());
        }

        unmap_and_close_peer(conn, is_server);

        // 从连接池中移除
        remove_connection_from_pools(conn);
    }

    // ------------------------------------------------------------
    // 配置加载
    // ------------------------------------------------------------

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
        routes_[route.match_pattern] = std::move(route);
        url_trie_.insert(route.match_pattern, true);

        LOG_INFO_TAG("add_route", "[Proxy] register route: pattern='{}', targets={}, balance={}", route.match_pattern, route.targets.size(), static_cast<int>(routes_[route.match_pattern].balance));

        for (const auto &tgt : routes_[route.match_pattern].targets) {
            LOG_INFO_TAG("add_route", "[Proxy] -> {}:{} (weight={})", tgt.host, tgt.port, tgt.weight);
        }
    }

    // ------------------------------------------------------------
    // URL 匹配和路由查询
    // ------------------------------------------------------------

    std::string HttpProxy::find_proxy_route(const std::string & url) const
    {
        auto result = url_trie_.find_prefix(url);
        if (!result || !result.is_registered)
            return "";
        return url.substr(0, static_cast<size_t>(result.match_length));
    }

    bool HttpProxy::is_proxy_url(const std::string & url) const
    {
        return !find_proxy_route(url).empty();
    }

    // ------------------------------------------------------------
    // 核心请求处理入口
    // ------------------------------------------------------------

    void HttpProxy::handle_websocket_upgrade_by_url(HttpRequest * req, HttpResponse * resp, const std::string & route_key)
    {
        if (!req || !resp || route_key.empty()) {
            if (resp)
                resp->process_error(ResponseCode::bad_gateway);
            return;
        }

        auto routeIt = routes_.find(route_key);
        if (routeIt == routes_.end()) {
            resp->process_error(ResponseCode::bad_gateway);
            return;
        }

        const ProxyRoute &route = routeIt->second;
        ProxyTarget target = select_target(route);
        handle_websocket_upgrade(req, resp, route, target);
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

        auto routeIt = routes_.find(route_key);
        if (routeIt == routes_.end()) {
            resp->process_error(ResponseCode::bad_gateway);
            ++stats_.failed_requests;
            return;
        }

        const ProxyRoute &route = routeIt->second;
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

        auto pool = get_or_create_pool(target, route);
        if (pool) {
            Connection *pooledConn = pool->acquire(this, server_);
            if (pooledConn) {
                ++stats_.pool_hits;
                map_connections(clientConn, pooledConn, route_key);
                req->pack_and_send(pooledConn);
                pooledConn->flush();
                return;
            }
        }

        ++stats_.pool_misses;

        Connection *remoteConn = create_remote_connection(target, route.connect_timeout_ms);
        if (!remoteConn) {
            resp->process_error(ResponseCode::bad_gateway);
            ++stats_.failed_requests;
            return;
        }

        ++stats_.active_connections;

        auto task = std::make_shared<RemoteConnectTask>(clientConn, req, resp, this, route_key, target);
        {
            std::lock_guard<std::mutex> lock(mapping_mutex_);
            pending_tasks_[clientConn] = task;
            pending_server_tasks_[remoteConn] = task;
            pending_requests_[clientConn] = 1;
        }

        if (server_ && server_->runtime()) {
            auto weak_task = std::weak_ptr<RemoteConnectTask>(task);
            auto *timer = server_->runtime()->schedule(
                static_cast<uint32_t>(route.connect_timeout_ms),
                [this, weak_task]() {
                    auto locked_task = weak_task.lock();
                    if (!locked_task) {
                        return;
                    }
                    on_connection_timeout(locked_task);
                    if (locked_task->resp_ && locked_task->client_conn_) {
                        locked_task->resp_->process_error(ResponseCode::gateway_timeout);
                    }
                });
            bind_timer(clientConn, timer);
            bind_timer(remoteConn, timer);
        }
    }

        // ------------------------------------------------------------
        // 连接建立完成回调
        // ------------------------------------------------------------
        void HttpProxy::on_connection_established(RemoteConnectTask::Ptr task, Connection * remoteConn)
        {
            if (!task || !remoteConn)
                return;

            // 取消超时定时�?
            timer::Timer *timer_to_cancel = take_timer(task->client_conn_);
            timer::Timer *remote_timer = take_timer(remoteConn);
            if (timer_to_cancel && remote_timer && timer_to_cancel != remote_timer) {
                remote_timer->cancel();
            } else if (!timer_to_cancel) {
                timer_to_cancel = remote_timer;
            }
            if (timer_to_cancel) {
                timer_to_cancel->cancel();
            }

            // 建立连接映射
            map_connections(task->client_conn_, remoteConn, task->route_key_);

            // 清理pending状态
            {
                std::lock_guard<std::mutex> lock(mapping_mutex_);
                pending_tasks_.erase(task->client_conn_);
                pending_requests_.erase(task->client_conn_);
                pending_server_tasks_.erase(remoteConn);
            }

            // 发送之前缓存的请求
            if (task->req_ && remoteConn) {
                task->req_->pack_and_send(remoteConn);
                remoteConn->flush();
            }
        }

        // ------------------------------------------------------------
        // 连接超时回调
        // ------------------------------------------------------------

        void HttpProxy::on_connection_timeout(RemoteConnectTask::Ptr task)
        {
            if (!task)
                return;

            LOG_WARN_TAG("on_connection_timeout", "[Proxy] connection timeout to {}:{}", task->target_.host, task->target_.port);

            Connection *clientConn = task->client_conn_;
            Connection *remoteConn = nullptr;

            // 查找对应的服务端连接（通过pending_server_tasks_反向查找）
            {
                std::lock_guard<std::mutex> lock(mapping_mutex_);
                // 从pending_tasks_中移�?
                pending_tasks_.erase(clientConn);

                // 找到远程连接
                for (auto it = pending_server_tasks_.begin(); it != pending_server_tasks_.end(); ++it) {
                    if (it->second.get() == task.get()) {
                        remoteConn = it->first;
                        pending_server_tasks_.erase(it);
                        break;
                    }
                }
            }

            // 清理定时器
            (void)take_timer(clientConn);
            if (remoteConn) {
                (void)take_timer(remoteConn);
            }

            // 关闭远程连接
            if (remoteConn) {
                // 从连接池中移除（如果有的话）
                remove_connection_from_pools(remoteConn);
                remoteConn->close();
            }

            --stats_.active_connections;
            ++stats_.failed_requests;
        }

        void HttpProxy::check_response_timer(Connection * conn)
        {
            // 检查连接是否还有待处理的响应定时器
            // 主要用于检测长时间无响应的代理请求
            std::lock_guard<std::mutex> lock(timer_mutex_);
            auto it = conn_timers_.find(conn);
            if (it != conn_timers_.end() && it->second) {
                // 定时器仍然存在，说明响应还未到达
                // 可以在这里添加额外检查逻辑（如更新超时时间等）
            }
        }

        // ------------------------------------------------------------
        // 客户端连接关闭
        // ------------------------------------------------------------

        void HttpProxy::on_client_close(Connection * conn)
        {
            unmap_and_close_peer(conn, true); // 客户端关闭
            std::lock_guard<std::mutex> lock(mapping_mutex_);
            pending_requests_.erase(conn);
        }

        // ------------------------------------------------------------
        // 负载均衡：选择目标后端
        // ------------------------------------------------------------

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
            // 遍历各目标对应的连接池，选活跃连接数最少的
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
                    // 没有池子说明还没有连接过，优先选择
                    best = target;
                    min_active = 0;
                    break;
                }
            }

            return best;
        }

        // ------------------------------------------------------------
        // 请求构建和头改写
        // ------------------------------------------------------------

        void HttpProxy::build_forward_request(HttpRequest * orig_req, const ProxyRoute & route,
                                              const ProxyTarget & target, bool is_websocket)
        {
            // 1. 设置正确的Host头
            orig_req->add_header("Host", target.host + ":" + std::to_string(target.port));

            // 2. 添加X-Forwarded-For（追加客户端IP）
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

            // 3. 添加X-Real-IP
            if (orig_req->get_context() && orig_req->get_context()->get_connection()) {
                const auto &addr = orig_req->get_context()->get_connection()->get_remote_address();
                orig_req->add_header("X-Real-IP", addr.to_address_key());
            }

            // 4. 添加X-Forwarded-Proto
            orig_req->add_header("X-Forwarded-Proto", "http");

            // 5. 移除hop-by-hop头（逐跳头不应被转发）
            // WebSocket升级请求必须保留 Connection 和 Upgrade 头
            if (!is_websocket) {
                orig_req->remove_header("connection");
                orig_req->remove_header("upgrade");
            }
            orig_req->remove_header("keep-alive");
            orig_req->remove_header("transfer-encoding");
            orig_req->remove_header("te");
            orig_req->remove_header("trailers");

            if (route.strip_prefix && !route.rewrite_prefix.empty()) {
            }
        }

        // ------------------------------------------------------------
        // 连接管理内部方法
        // ------------------------------------------------------------

        Connection *HttpProxy::create_remote_connection(const ProxyTarget & target, int timeout_ms)
        {
            auto sock = new net::Socket(target.host.c_str(), target.port);
            if (!sock->valid()) {
                delete sock;
                LOG_ERROR_TAG("create_remote_connection", "[Proxy] create socket failed: {}:{}", target.host, target.port);
                return nullptr;
            }

            if (!sock->connect()) {
                delete sock;
                LOG_ERROR_TAG("create_remote_connection", "[Proxy] connect failed: {}:{}", target.host, target.port);
                return nullptr;
            }

            auto conn = create_stream_connection(sock);

            if (server_ && server_->runtime()) {
                server_->runtime()->register_connection(conn, this);
            }

            return conn;
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

        void HttpProxy::unmap_and_close_peer(Connection * conn, bool is_client)
        {
            Connection *peer_to_close = nullptr;
            {
                std::lock_guard<std::mutex> lock(mapping_mutex_);

                if (is_client) {
                    // 客户端关闭 -> 关闭对应的服务端连接
                    auto it = cs_mapping_.find(conn);
                    if (it != cs_mapping_.end()) {
                        Connection *serverConn = it->second;
                        cs_mapping_.erase(it);

                        if (serverConn) {
                            auto sit = sc_mapping_.find(serverConn);
                            if (sit != sc_mapping_.end()) {
                                sc_mapping_.erase(sit);
                            }
                            peer_to_close = serverConn;
                        }
                    }
                } else {
                    // 服务端关闭 -> 关闭对应的客户端连接
                    auto it = sc_mapping_.find(conn);
                    if (it != sc_mapping_.end()) {
                        Connection *clientConn = it->second.client_conn;
                        sc_mapping_.erase(it);

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

        // ------------------------------------------------------------
        // WebSocket升级支持
        // ------------------------------------------------------------

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

            auto *ctx = req->get_context();
            Connection *clientConn = ctx ? ctx->get_connection() : nullptr;
            if (!clientConn) {
                resp->process_error(ResponseCode::internal_server_error);
                ++stats_.failed_requests;
                return true;
            }

            Connection *remoteConn = create_remote_connection(target, route.connect_timeout_ms);
            if (!remoteConn) {
                resp->process_error(ResponseCode::bad_gateway);
                ++stats_.failed_requests;
                return true;
            }

            ++stats_.active_connections;

            const std::string route_key = route.match_pattern;
            map_connections(clientConn, remoteConn, route_key);

            req->pack_and_send(remoteConn);
            remoteConn->flush();

            return true;
        }

        timer::Timer *HttpProxy::take_timer(Connection * conn)
        {
            std::lock_guard<std::mutex> lock(timer_mutex_);
            auto it = conn_timers_.find(conn);
            if (it == conn_timers_.end()) {
                return nullptr;
            }
            auto *timer = it->second;
            conn_timers_.erase(it);
            return timer;
        }

        void HttpProxy::bind_timer(Connection * conn, timer::Timer * timer)
        {
            std::lock_guard<std::mutex> lock(timer_mutex_);
            conn_timers_[conn] = timer;
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

        // ------------------------------------------------------------
        // 连接池管理公共接�?// ------------------------------------------------------------

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
                // 双重检查，避免重复创建池子
                auto it = pools_.find(pool_id);
                if (it != pools_.end() && it->second) {
                    return it->second; // 另一个线程已经创建了
                }
                pools_[pool_id] = pool;
            }

            return pool;
        }

    } // namespace yuan::net::http
