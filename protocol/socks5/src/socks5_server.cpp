#include "socks5_server.h"
#include "socks5_packet_parser.h"
#include "net/acceptor/datagram_acceptor.h"
#include "net/acceptor/acceptor_factory.h"
#include "net/connection/connection.h"
#include "net/connection/tcp_connection.h"
#include "net/connection/udp_connection.h"
#include "net/connection/stream_transport.h"
#include "net/acceptor/udp/udp_instance.h"
#include "net/runtime/network_runtime.h"
#include "net/ip_policy.h"
#include "coroutine/connect_awaitable.h"
#include "coroutine/stream_io_awaitable.h"
#include "coroutine/io_result.h"
#include "net/socket/socket.h"
#include "net/socket/inet_address.h"
#include "logger.h"

#include <algorithm>
#include <cstring>
#include <optional>
#include <chrono>
#include <thread>

namespace
{
    struct ActiveSessionGuard
    {
        explicit ActiveSessionGuard(std::atomic_int &counter) : counter_(counter)
        {
            counter_.fetch_add(1, std::memory_order_relaxed);
        }

        ~ActiveSessionGuard()
        {
            counter_.fetch_sub(1, std::memory_order_relaxed);
        }

        std::atomic_int &counter_;
    };

    class RelayEndpointHandler final : public ::yuan::net::ConnectionHandler
    {
    public:
        explicit RelayEndpointHandler(std::atomic_bool *alive) noexcept
            : alive_(alive)
        {
        }

        void on_connected(::yuan::net::Connection &conn) override
        {
            (void)conn;
        }

        void on_error(::yuan::net::Connection &conn) override
        {
            (void)conn;
            if (alive_) {
                alive_->store(false, std::memory_order_release);
            }
        }

        void on_read(::yuan::net::Connection &conn) override
        {
            (void)conn;
        }

        void on_write(::yuan::net::Connection &conn) override
        {
            (void)conn;
        }

        void on_close(::yuan::net::Connection &conn) override
        {
            (void)conn;
            if (alive_) {
                alive_->store(false, std::memory_order_release);
            }
        }

    private:
        std::atomic_bool *alive_ = nullptr;
    };

    struct RelayBridge
    {
        std::atomic_bool client_alive{true};
        std::atomic_bool remote_alive{true};
        std::shared_ptr<::yuan::net::Connection> client_conn;
        std::shared_ptr<::yuan::net::Connection> remote_conn;
        RelayEndpointHandler client_handler{ &client_alive };
        RelayEndpointHandler remote_handler{ &remote_alive };
        std::atomic_uint64_t bytes_up{0};
        std::atomic_uint64_t bytes_down{0};
        std::mutex close_reason_mutex;
        std::string close_reason{"closed"};

        void note_close_reason(const std::string &reason)
        {
            std::lock_guard<std::mutex> lock(close_reason_mutex);
            if (close_reason == "closed") {
                close_reason = reason;
            }
        }

        void close_client()
        {
            if (client_conn && client_alive.exchange(false, std::memory_order_acq_rel)) {
                client_conn->close();
            }
        }

        void close_remote()
        {
            if (remote_conn && remote_alive.exchange(false, std::memory_order_acq_rel)) {
                remote_conn->close();
            }
        }

        void close_both()
        {
            close_client();
            close_remote();
        }
    };

    bool wildcard_match(std::string_view pattern, std::string_view value)
    {
        std::size_t p = 0;
        std::size_t v = 0;
        std::size_t star = std::string_view::npos;
        std::size_t match = 0;

        while (v < value.size()) {
            if (p < pattern.size() && (pattern[p] == '?' || pattern[p] == value[v])) {
                ++p;
                ++v;
            } else if (p < pattern.size() && pattern[p] == '*') {
                star = p++;
                match = v;
            } else if (star != std::string_view::npos) {
                p = star + 1;
                v = ++match;
            } else {
                return false;
            }
        }

        while (p < pattern.size() && pattern[p] == '*') {
            ++p;
        }
        return p == pattern.size();
    }

    std::string to_lower(std::string value)
    {
        for (char &ch : value) {
            ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        }
        return value;
    }

    std::string trim(std::string value)
    {
        std::size_t begin = 0;
        while (begin < value.size() && std::isspace(static_cast<unsigned char>(value[begin]))) {
            ++begin;
        }
        std::size_t end = value.size();
        while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1]))) {
            --end;
        }
        return value.substr(begin, end - begin);
    }

    bool match_rule(const std::string &rule, const std::string &host, uint16_t port)
    {
        const auto colon = rule.rfind(':');
        std::string host_rule = to_lower(trim(rule));
        std::string port_rule = "*";
        if (colon != std::string::npos && rule.find(':') == colon) {
            host_rule = to_lower(trim(rule.substr(0, colon)));
            port_rule = trim(rule.substr(colon + 1));
        }
        const bool host_ok = wildcard_match(host_rule.empty() ? "*" : host_rule, to_lower(host));
        const bool port_ok = port_rule == "*" || (!port_rule.empty() && static_cast<uint16_t>(std::stoi(port_rule)) == port);
        return host_ok && port_ok;
    }

    bool target_allowed(const yuan::net::socks5::Socks5ServerConfig &config, const std::string &host, uint16_t port)
    {
        for (const auto &rule : config.deny_targets) {
            if (!rule.empty() && match_rule(rule, host, port)) {
                return false;
            }
        }
        if (config.allow_targets.empty()) {
            return true;
        }
        for (const auto &rule : config.allow_targets) {
            if (!rule.empty() && match_rule(rule, host, port)) {
                return true;
            }
        }
        return false;
    }

    const char *socks5_session_state_name(yuan::net::socks5::Socks5Session::State state) noexcept
    {
        using S = yuan::net::socks5::Socks5Session::State;
        switch (state) {
        case S::greeting: return "greeting";
        case S::auth: return "auth";
        case S::request: return "request";
        case S::connecting: return "connecting";
        case S::udp_associate: return "udp_associate";
        case S::established: return "established";
        case S::closed: return "closed";
        }
        return "unknown";
    }

    enum class ParseProbeResult {
        incomplete,
        malformed,
        complete
    };

    ParseProbeResult probe_greeting(const ::yuan::buffer::ByteBuffer &buf, std::size_t &consumed)
    {
        auto span = buf.readable_span();
        if (span.size() < 2) {
            return ParseProbeResult::incomplete;
        }

        const auto *data = reinterpret_cast<const uint8_t *>(span.data());
        if (data[0] != static_cast<uint8_t>(::yuan::net::socks5::SocksVersion::v5)) {
            return ParseProbeResult::malformed;
        }

        const std::size_t method_count = data[1];
        if (span.size() < 2 + method_count) {
            return ParseProbeResult::incomplete;
        }

        auto greeting = ::yuan::net::socks5::Socks5PacketParser::parse_greeting(buf);
        if (!greeting) {
            return ParseProbeResult::malformed;
        }

        consumed = 2 + method_count;
        return ParseProbeResult::complete;
    }

    ParseProbeResult probe_auth_request(const ::yuan::buffer::ByteBuffer &buf, std::size_t &consumed)
    {
        auto span = buf.readable_span();
        if (span.size() < 2) {
            return ParseProbeResult::incomplete;
        }

        const auto *data = reinterpret_cast<const uint8_t *>(span.data());
        if (data[0] != 0x01) {
            return ParseProbeResult::malformed;
        }

        const std::size_t ulen = data[1];
        if (span.size() < 2 + ulen + 1) {
            return ParseProbeResult::incomplete;
        }

        const std::size_t plen = data[2 + ulen];
        if (span.size() < 3 + ulen + plen) {
            return ParseProbeResult::incomplete;
        }

        auto auth_result = ::yuan::net::socks5::Socks5PacketParser::parse_auth_request(buf);
        if (!auth_result) {
            return ParseProbeResult::malformed;
        }

        consumed = 3 + ulen + plen;
        return ParseProbeResult::complete;
    }

    ParseProbeResult probe_request(const ::yuan::buffer::ByteBuffer &buf, std::size_t &consumed)
    {
        auto span = buf.readable_span();
        if (span.size() < 4) {
            return ParseProbeResult::incomplete;
        }

        const auto *data = reinterpret_cast<const uint8_t *>(span.data());
        if (data[0] != static_cast<uint8_t>(::yuan::net::socks5::SocksVersion::v5)) {
            return ParseProbeResult::malformed;
        }

        const auto atyp = static_cast<::yuan::net::socks5::AddressType>(data[3]);
        std::size_t total_needed = 0;
        switch (atyp) {
        case ::yuan::net::socks5::AddressType::ipv4:
            total_needed = 10;
            break;
        case ::yuan::net::socks5::AddressType::domain:
            if (span.size() < 5) {
                return ParseProbeResult::incomplete;
            }
            total_needed = 7 + data[4];
            break;
        case ::yuan::net::socks5::AddressType::ipv6:
            total_needed = 22;
            break;
        default:
            return ParseProbeResult::malformed;
        }

        if (span.size() < total_needed) {
            return ParseProbeResult::incomplete;
        }

        auto request = ::yuan::net::socks5::Socks5PacketParser::parse_request(buf);
        if (!request) {
            return ParseProbeResult::malformed;
        }

        consumed = total_needed;
        return ParseProbeResult::complete;
    }
} // namespace

namespace yuan::net::socks5
{
    Socks5Server::Socks5Server()
        : udp_relay_handler_(*this)
    {
    }

    Socks5Server::Socks5Server(const Socks5ServerConfig & config)
        : config_(config),
          udp_relay_handler_(*this)
    {
    }

    Socks5Server::~Socks5Server()
    {
        stop();

        for (auto & [
                        conn,
                        assoc
                    ] : udp_associations_) {
            if (assoc) {
                if (assoc->idle_timer) {
                    assoc->idle_timer.cancel();
                }
                if (assoc->udp_acceptor) {
                    assoc->udp_acceptor->close();
                }
            }
        }
        udp_associations_.clear();
        udp_conn_to_client_.clear();

        listener_.close();
        ssl_module_.reset();
    }

    bool Socks5Server::init(int port)
    {
        owned_runtime_ = std::make_unique<NetworkRuntime>();
        return init(config_.listen_host, port, *owned_runtime_);
    }

    bool Socks5Server::init(int port, NetworkRuntime & runtime)
    {
        return init(config_.listen_host, port, runtime);
    }

    bool Socks5Server::init(const std::string &host, int port)
    {
        owned_runtime_ = std::make_unique<NetworkRuntime>();
        return init(host, port, *owned_runtime_);
    }

    bool Socks5Server::init(const std::string &host, int port, NetworkRuntime & runtime)
    {
        if (port <= 0 || port > 65535) {
            LOG_ERROR("socks5 server: invalid port={}, must be 1-65535", port);
            return false;
        }
        if (config_.max_connections == 0) {
            LOG_WARN("socks5 server: max_connections=0 invalid, defaulting to 8192");
            config_.max_connections = 8192;
        }
        if (config_.connect_timeout_ms == 0) {
            LOG_WARN("socks5 server: connect_timeout_ms=0 invalid, defaulting to 10000");
            config_.connect_timeout_ms = 10000;
        }
        if (config_.idle_timeout_ms == 0) {
            LOG_WARN("socks5 server: idle_timeout_ms=0 invalid, defaulting to 300000");
            config_.idle_timeout_ms = 300000;
        }
        if (config_.enable_auth && config_.username.empty()) {
            LOG_WARN("socks5 server: auth enabled but username is empty");
        }
        if (!config_.allow_targets.empty() && !config_.deny_targets.empty()) {
            LOG_WARN("socks5 server: both allow_targets and deny_targets are set; deny is evaluated first, then allow acts as whitelist");
        }

        stop_requested_.store(false, std::memory_order_relaxed);
        listener_.close();

        if (idle_sweep_timer_) {
            idle_sweep_timer_.cancel();
            idle_sweep_timer_.reset();
        }

        if (ssl_module_) {
            listener_.set_ssl_module(ssl_module_);
        }

        if (!listener_.bind(host, static_cast<uint16_t>(port), runtime)) {
            LOG_ERROR("socks5 server: failed to bind on {}:{}",
                      host.empty() ? "0.0.0.0" : host, port);
            if (owned_runtime_)
                owned_runtime_.reset();
            return false;
        }

        LOG_INFO("socks5 server: initialized on {}:{}, max_connections={}, auth={}, connect={}, udp={}",
                  host.empty() ? "0.0.0.0" : host, port,
                  config_.max_connections,
                  config_.enable_auth ? "on" : "off",
                  config_.enable_connect ? "on" : "off",
                  config_.enable_udp_associate ? "on" : "off");
        return true;
    }

    void Socks5Server::serve()
    {
        auto *runtime = listener_.runtime();

        if (runtime && config_.idle_timeout_ms > 0) {
            idle_sweep_timer_ = runtime->schedule_periodic(
                1000, 1000,
                [this]() {
                    if (metrics_.active_sessions.load(std::memory_order_relaxed) == 0) {
                        return;
                    }
                    LOG_INFO("socks5 server: periodic stats active={} accepted={} completed={} "
                             "connect_timeouts={} idle_timeouts={} by_client={} by_upstream={} by_ssrf={} by_acl={}",
                             metrics_.active_sessions.load(std::memory_order_relaxed),
                             metrics_.accepted_sessions.load(std::memory_order_relaxed),
                             metrics_.completed_sessions.load(std::memory_order_relaxed),
                             metrics_.connect_timeouts.load(std::memory_order_relaxed),
                             metrics_.idle_timeouts.load(std::memory_order_relaxed),
                             metrics_.closes_by_client.load(std::memory_order_relaxed),
                             metrics_.closes_by_upstream.load(std::memory_order_relaxed),
                             metrics_.closes_by_ssrf.load(std::memory_order_relaxed),
                             metrics_.closes_by_acl.load(std::memory_order_relaxed));
                });
        }

        auto accept_loop = [this]() -> coroutine::Task<void> {
            auto *acceptor_runtime = listener_.runtime();
            auto acceptor = listener_.acceptor();
            if (!acceptor_runtime || !acceptor) {
                co_return;
            }

            auto rv = acceptor_runtime->runtime_view();
            while (!stop_requested_.load(std::memory_order_relaxed)) {
                auto conn = co_await coroutine::async_accept(rv, acceptor.get());
                if (!conn) {
                    break;
                }

                auto *event_loop = rv.event_loop();
                if (!event_loop) {
                    conn->close();
                    continue;
                }

                if (config_.max_connections > 0 &&
                    metrics_.active_sessions.load(std::memory_order_relaxed) >=
                        static_cast<int>(config_.max_connections)) {
                    LOG_WARN("socks5 server: rejecting connection, max_connections {} reached",
                             config_.max_connections);
                    metrics_.rejected_sessions.fetch_add(1, std::memory_order_relaxed);
                    conn->close();
                    continue;
                }

                if (config_.max_sessions_per_client > 0) {
                    const std::string client_ip = conn->get_remote_address().get_ip();
                    size_t current_count = 0;
                    {
                        std::lock_guard<std::mutex> lock(tcp_sessions_mutex_);
                        auto it = tcp_sessions_per_client_.find(client_ip);
                        if (it != tcp_sessions_per_client_.end()) {
                            current_count = it->second;
                        }
                    }
                    if (current_count >= config_.max_sessions_per_client) {
                        LOG_WARN("socks5 server: rejecting client {}, per-client limit {} reached",
                                 client_ip, config_.max_sessions_per_client);
                        metrics_.rejected_sessions.fetch_add(1, std::memory_order_relaxed);
                        if (session_event_cb_) {
                            Socks5SessionInfo info;
                            info.client_addr = conn->get_remote_address().to_address_key();
                            info.command = "connect";
                            session_event_cb_("rejected", info);
                        }
                        conn->close();
                        continue;
                    }
                }

                auto ctx = AsyncConnectionContext(conn, rv);
                if (!ctx.runtime_view().event_loop()) {
                    conn->close();
                    continue;
                }

                auto task = handle_connection(std::move(ctx));
                task.resume();
                task.detach();
            }
            co_return;
        };

        accept_task_ = accept_loop();
        accept_task_.resume();

        if (owned_runtime_) {
            owned_runtime_->run();
        }
    }

    void Socks5Server::stop()
    {
        stop_requested_.store(true, std::memory_order_relaxed);

        auto close_listener_and_timers = [this]() {
            listener_.close();
            if (idle_sweep_timer_) {
                idle_sweep_timer_.cancel();
                idle_sweep_timer_.reset();
            }
        };

        if (owned_runtime_ && owned_runtime_->event_loop()) {
            owned_runtime_->dispatch(close_listener_and_timers);
        } else {
            close_listener_and_timers();
        }

        const auto drain_ms = std::max(static_cast<uint32_t>(config_.drain_timeout_ms), uint32_t(0));
        if (drain_ms > 0 && metrics_.active_sessions.load(std::memory_order_relaxed) > 0) {
            LOG_INFO("socks5 server: draining {} active sessions, timeout={}ms",
                     metrics_.active_sessions.load(std::memory_order_relaxed), drain_ms);
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(drain_ms);
            while (std::chrono::steady_clock::now() < deadline) {
                if (metrics_.active_sessions.load(std::memory_order_relaxed) == 0) {
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
        }

        const int remaining = metrics_.active_sessions.load(std::memory_order_relaxed);
        if (remaining > 0) {
            LOG_WARN("socks5 server: {} sessions did not drain within {}ms",
                     remaining, drain_ms);
        }

        if (owned_runtime_) {
            owned_runtime_->stop();
        }

        accept_task_ = {};
    }

    coroutine::Task<void> Socks5Server::handle_connection(AsyncConnectionContext ctx)
    {
        auto client_conn = ctx.connection();
        ActiveSessionGuard active_guard(metrics_.active_sessions);
        const auto started_at = std::chrono::steady_clock::now();
        const std::string client_addr = client_conn->get_remote_address().to_address_key();
        const std::string client_ip = client_conn->get_remote_address().get_ip();

        if (config_.enable_auth && auth_rate_limiter_.is_banned(client_ip)) {
            LOG_WARN("socks5 server: rejecting banned client {} due to auth rate limit", client_ip);
            metrics_.rejected_sessions.fetch_add(1, std::memory_order_relaxed);
            ctx.close();
            co_return;
        }

        if (config_.max_sessions_per_client > 0) {
            std::lock_guard<std::mutex> lock(tcp_sessions_mutex_);
            tcp_sessions_per_client_[client_ip]++;
        }

        struct TcpClientGuard {
            std::string ip;
            std::unordered_map<std::string, size_t> &map;
            std::mutex &mtx;
            size_t limit;
            bool active;

            ~TcpClientGuard() {
                if (limit > 0 && active) {
                    std::lock_guard<std::mutex> lock(mtx);
                    auto it = map.find(ip);
                    if (it != map.end()) {
                        if (it->second > 0) {
                            --it->second;
                        }
                        if (it->second == 0) {
                            map.erase(it);
                        }
                    }
                }
            }
        };

        TcpClientGuard tcp_client_guard{client_ip, tcp_sessions_per_client_, tcp_sessions_mutex_, config_.max_sessions_per_client, config_.max_sessions_per_client > 0};

        auto fire_state_change = [&](Socks5Session::State previous, Socks5Session::State current, const std::string &reason) {
            if (session_state_cb_) {
                session_state_cb_("state_changed", client_addr,
                                  socks5_session_state_name(previous),
                                  socks5_session_state_name(current),
                                  reason);
            }
        };

        Socks5Session session(client_conn);
        auto prev_state = session.state();
        ::yuan::buffer::ByteBuffer pending;
        auto read_more = [&]() -> coroutine::Task<bool> {
            auto read_result = co_await ctx.read_awaiter();
            if (read_result.status != coroutine::IoStatus::success) {
                co_return false;
            }
            pending.append(read_result.data);
            co_return true;
        };

        std::size_t consumed = 0;
        while (true) {
            auto probe = probe_greeting(pending, consumed);
            if (probe == ParseProbeResult::complete) {
                break;
            }
            if (probe == ParseProbeResult::malformed) {
                ctx.close();
                co_return;
            }
            if (!co_await read_more()) {
                co_return;
            }
        }

        auto greeting = Socks5PacketParser::parse_greeting(pending);
        if (!greeting) {
            ctx.close();
            co_return;
        }
        pending.consume(consumed);
        bool support_no_auth = false;
        bool support_user_pass = false;
        for (uint8_t i = 0; i < greeting->method_count; ++i) {
            if (greeting->methods[i] == static_cast<uint8_t>(AuthMethod::no_auth)) {
                support_no_auth = true;
            }
            if (greeting->methods[i] == static_cast<uint8_t>(AuthMethod::username_password)) {
                support_user_pass = true;
            }
        }

        AuthMethod selected = AuthMethod::no_acceptable;
        if (config_.enable_auth && support_user_pass) {
            selected = AuthMethod::username_password;
            prev_state = session.state();
            session.set_state(Socks5Session::State::auth);
            fire_state_change(prev_state, Socks5Session::State::auth, "auth_started");
        } else if (!config_.enable_auth && support_no_auth) {
            selected = AuthMethod::no_auth;
            prev_state = session.state();
            session.set_state(Socks5Session::State::request);
            fire_state_change(prev_state, Socks5Session::State::request, "no_auth");
        }

        auto method_reply = Socks5PacketParser::build_method_select_reply(selected);
        ctx.write_and_flush(method_reply);
        if (selected == AuthMethod::no_acceptable) {
            metrics_.rejected_sessions.fetch_add(1, std::memory_order_relaxed);
            if (session_event_cb_) {
                Socks5SessionInfo info;
                info.client_addr = client_addr;
                info.command = "greeting";
                session_event_cb_("rejected", info);
            }
            ctx.close();
            co_return;
        }

        if (selected == AuthMethod::username_password) {
            consumed = 0;
            while (true) {
                auto probe = probe_auth_request(pending, consumed);
                if (probe == ParseProbeResult::complete) {
                    break;
                }
                if (probe == ParseProbeResult::malformed) {
                    ctx.close();
                    co_return;
                }
                if (!co_await read_more()) {
                    co_return;
                }
            }

            auto auth_result = Socks5PacketParser::parse_auth_request(pending);
            if (!auth_result) {
                ctx.close();
                co_return;
            }
            pending.consume(consumed);

            session.set_username(auth_result->first);
            session.set_password(auth_result->second);

            bool auth_ok = false;
            if (handler_) {
                auth_ok = handler_->on_authenticate(auth_result->first, auth_result->second);
            } else {
                auth_ok = (auth_result->first == config_.username && auth_result->second == config_.password);
            }

            auto auth_reply = Socks5PacketParser::build_auth_reply(auth_ok);
            ctx.write_and_flush(auth_reply);

            if (!auth_ok) {
                auth_rate_limiter_.record_failure(client_ip);
                metrics_.rejected_sessions.fetch_add(1, std::memory_order_relaxed);
                if (session_event_cb_) {
                    Socks5SessionInfo info;
                    info.client_addr = client_addr;
                    info.command = "auth";
                    session_event_cb_("rejected", info);
                }
                ctx.close();
                co_return;
            }

            auth_rate_limiter_.record_success(client_ip);
            session.set_state(Socks5Session::State::request);
        }

        consumed = 0;
        while (true) {
            auto probe = probe_request(pending, consumed);
            if (probe == ParseProbeResult::complete) {
                break;
            }
            if (probe == ParseProbeResult::malformed) {
                send_reply(client_conn, ReplyCode::general_failure);
                ctx.close();
                co_return;
            }
            if (!co_await read_more()) {
                co_return;
            }
        }

        auto request = Socks5PacketParser::parse_request(pending);
        if (!request) {
            send_reply(client_conn, ReplyCode::general_failure);
            ctx.close();
            co_return;
        }
        pending.consume(consumed);

        session.set_command(request->cmd);
        session.set_address_type(request->atyp);
        session.set_target_host(request->address);
        session.set_target_port(request->port);

        std::string command_name;
        switch (request->cmd) {
        case Command::connect: command_name = "connect"; break;
        case Command::bind: command_name = "bind"; break;
        case Command::udp_associate: command_name = "udp_associate"; break;
        default: command_name = "unknown"; break;
        }
        const std::string target_addr = session.target_host() + ":" + std::to_string(session.target_port());

        auto fire_rejected = [&](const std::string &reason) {
            metrics_.rejected_sessions.fetch_add(1, std::memory_order_relaxed);
            if (session_event_cb_) {
                Socks5SessionInfo info;
                info.client_addr = client_addr;
                info.command = command_name;
                info.target_addr = target_addr;
                session_event_cb_("rejected", info);
            }
        };

        auto fire_accepted = [&]() {
            metrics_.accepted_sessions.fetch_add(1, std::memory_order_relaxed);
            if (session_event_cb_) {
                Socks5SessionInfo info;
                info.client_addr = client_addr;
                info.command = command_name;
                info.target_addr = target_addr;
                session_event_cb_("accepted", info);
            }
        };

        auto fire_completed = [&](uint64_t bytes_up, uint64_t bytes_down, const std::string &close_reason) {
            metrics_.completed_sessions.fetch_add(1, std::memory_order_relaxed);
            metrics_.bytes_up.fetch_add(bytes_up, std::memory_order_relaxed);
            metrics_.bytes_down.fetch_add(bytes_down, std::memory_order_relaxed);
            if (session_event_cb_) {
                Socks5SessionInfo info;
                info.client_addr = client_addr;
                info.command = command_name;
                info.target_addr = target_addr;
                info.duration_ms = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - started_at).count());
                info.bytes_up = bytes_up;
                info.bytes_down = bytes_down;
                info.close_reason = close_reason;
                session_event_cb_("completed", info);
            }
        };

        switch (request->cmd) {
        case Command::connect: {
            if (!config_.enable_connect) {
                send_reply(client_conn, ReplyCode::command_not_supported);
                fire_rejected("command_not_supported");
                ctx.close();
                co_return;
            }

            if (!target_allowed(config_, session.target_host(), session.target_port())) {
                LOG_WARN("socks5 server: target denied by ACL {}:{}",
                         session.target_host(), session.target_port());
                send_reply(client_conn, ReplyCode::connection_not_allowed);
                metrics_.closes_by_acl.fetch_add(1, std::memory_order_relaxed);
                fire_rejected("acl_denied");
                ctx.close();
                co_return;
            }

            if (handler_) {
                if (!handler_->on_connect_request(&session, session.target_host(), session.target_port())) {
                    send_reply(client_conn, ReplyCode::connection_not_allowed);
                    fire_rejected("handler_denied");
                    ctx.close();
                    co_return;
                }
            }

            fire_state_change(prev_state, Socks5Session::State::connecting, "connect_started");

            auto rv = ctx.runtime_view();
            auto connect_result = co_await coroutine::async_connect(
                rv, session.target_host(), session.target_port(), config_.connect_timeout_ms);

            if (connect_result.result != coroutine::ConnectResult::success || !connect_result.connection) {
                if (connect_result.result == coroutine::ConnectResult::timed_out) {
                    metrics_.connect_timeouts.fetch_add(1, std::memory_order_relaxed);
                }
                ReplyCode reply = ReplyCode::general_failure;
                if (connect_result.result == coroutine::ConnectResult::timed_out) {
                    reply = ReplyCode::ttl_expired;
                } else if (connect_result.result == coroutine::ConnectResult::connection_error ||
                           connect_result.result == coroutine::ConnectResult::connect_failed) {
                    reply = ReplyCode::connection_refused;
                }
                send_reply(client_conn, reply);
                fire_rejected("upstream_connect_failed");
                ctx.close();
                co_return;
            }

            auto remote_conn = connect_result.connection;

            if (!config_.allow_private_targets) {
                const std::string resolved_ip = remote_conn->get_remote_address().get_ip();
                if (is_private_ip(resolved_ip)) {
                    LOG_WARN("socks5 server: SSRF blocked, target {} resolves to private IP {}",
                             session.target_host(), resolved_ip);
                    send_reply(client_conn, ReplyCode::connection_not_allowed);
                    metrics_.closes_by_ssrf.fetch_add(1, std::memory_order_relaxed);
                    fire_rejected("ssrf_blocked");
                    remote_conn->close();
                    ctx.close();
                    co_return;
                }
            }

            auto bridge = std::make_shared<RelayBridge>();
            bridge->client_conn = client_conn;
            bridge->remote_conn = remote_conn;

            rv.register_connection(remote_conn, make_non_owning_handler(&bridge->remote_handler));
            if (auto stream = std::dynamic_pointer_cast<StreamTransport>(remote_conn)) {
                if (auto *channel = stream->stream_channel()) {
                    rv.update_channel(channel);
                }
            }

            session.set_remote_connection(remote_conn);
            session.set_state(Socks5Session::State::established);
            fire_state_change(Socks5Session::State::connecting, Socks5Session::State::established, "connect_succeeded");

            const auto &remote_addr = remote_conn->get_remote_address();
            auto reply = Socks5PacketParser::build_reply(
                ReplyCode::succeeded, AddressType::ipv4,
                remote_addr.get_ip(), remote_addr.get_port());
            ctx.write_and_flush(reply);

            if (handler_) {
                handler_->on_session_opened(&session);
            }

            fire_accepted();

            LOG_INFO("socks5 server: session established -> {}:{}",
                     session.target_host(), session.target_port());

            client_conn->set_connection_handler(make_aliasing_handler(bridge, &bridge->client_handler));

            auto on_relay_close = [bridge](const std::string &reason) {
                bridge->note_close_reason(reason);
                bridge->close_both();
            };

            auto t1 = relay_pipe(rv, client_conn, remote_conn, on_relay_close, &bridge->remote_alive, &bridge->bytes_up);
            t1.resume();
            t1.detach();

            auto t2 = relay_pipe(rv, remote_conn, client_conn, on_relay_close, &bridge->client_alive, &bridge->bytes_down);
            co_await t2;

            {
                const std::string reason = bridge->close_reason;
                if (reason.find("timeout") != std::string::npos) {
                    metrics_.idle_timeouts.fetch_add(1, std::memory_order_relaxed);
                } else if (!bridge->client_alive.load(std::memory_order_acquire)) {
                    metrics_.closes_by_client.fetch_add(1, std::memory_order_relaxed);
                } else if (!bridge->remote_alive.load(std::memory_order_acquire)) {
                    metrics_.closes_by_upstream.fetch_add(1, std::memory_order_relaxed);
                }
            }

            if (handler_) {
                handler_->on_session_closed(&session);
            }

            fire_completed(
                bridge->bytes_up.load(std::memory_order_relaxed),
                bridge->bytes_down.load(std::memory_order_relaxed),
                bridge->close_reason);

            fire_state_change(Socks5Session::State::established, Socks5Session::State::closed, bridge->close_reason);
            co_return;
        }

        case Command::bind: {
            if (!config_.enable_bind) {
                send_reply(client_conn, ReplyCode::command_not_supported);
                fire_rejected("command_not_supported");
                ctx.close();
                co_return;
            }
            send_reply(client_conn, ReplyCode::command_not_supported);
            fire_rejected("command_not_supported");
            ctx.close();
            co_return;
        }

        case Command::udp_associate: {
            if (!config_.enable_udp_associate) {
                send_reply(client_conn, ReplyCode::command_not_supported);
                fire_rejected("command_not_supported");
                ctx.close();
                co_return;
            }

            if (!target_allowed(config_, session.target_host(), session.target_port())) {
                LOG_WARN("socks5 server: UDP target denied by ACL {}:{}",
                         session.target_host(), session.target_port());
                send_reply(client_conn, ReplyCode::connection_not_allowed);
                metrics_.closes_by_acl.fetch_add(1, std::memory_order_relaxed);
                fire_rejected("acl_denied");
                ctx.close();
                co_return;
            }

            if (handler_) {
                if (!handler_->on_connect_request(&session, session.target_host(), session.target_port())) {
                    send_reply(client_conn, ReplyCode::connection_not_allowed);
                    fire_rejected("handler_denied");
                    ctx.close();
                    co_return;
                }
            }

            const std::string client_ip = client_conn->get_remote_address().get_ip();
            if (config_.max_udp_associations_per_client > 0) {
                size_t current_count = 0;
                auto it = udp_associations_per_client_.find(client_ip);
                if (it != udp_associations_per_client_.end()) {
                    current_count = it->second;
                }
                if (current_count >= config_.max_udp_associations_per_client) {
                    LOG_WARN("socks5 server: UDP associate rejected for client {}, limit {} reached",
                             client_ip, config_.max_udp_associations_per_client);
                    send_reply(client_conn, ReplyCode::connection_not_allowed);
                    fire_rejected("udp_per_client_limit");
                    ctx.close();
                    co_return;
                }
            }

            Socket *udp_sock = new Socket("", 0, true);
            udp_sock->set_none_block(true);
            if (!udp_sock->valid()) {
                delete udp_sock;
                LOG_ERROR("socks5 server: failed to create UDP socket for associate");
                send_reply(client_conn, ReplyCode::general_failure);
                ctx.close();
                co_return;
            }

            udp_sock->set_reuse(true);
            if (!udp_sock->bind()) {
                delete udp_sock;
                LOG_ERROR("socks5 server: failed to bind UDP socket for associate");
                send_reply(client_conn, ReplyCode::general_failure);
                ctx.close();
                co_return;
            }

            auto *runtime = listener_.runtime();
            auto udp_acceptor = std::unique_ptr<DatagramAcceptor>(create_datagram_acceptor(udp_sock, *runtime));
            if (!udp_acceptor->listen()) {
                LOG_ERROR("socks5 server: failed to listen on UDP socket for associate");
                send_reply(client_conn, ReplyCode::general_failure);
                ctx.close();
                co_return;
            }

            runtime->register_acceptor(udp_acceptor, make_non_owning_handler(&udp_relay_handler_), udp_acceptor->endpoint_channel());

            auto assoc = std::make_unique<UdpAssociation>();
            assoc->client_conn = client_conn;
            assoc->udp_endpoint = udp_acceptor ? &*udp_acceptor : nullptr;
            assoc->udp_acceptor = std::move(udp_acceptor);
            assoc->idle_timer.reset();
            assoc->client_ip = client_ip;

            if (session.target_host().empty() || session.target_port() == 0) {
                assoc->client_udp_addr = InetAddress();
            } else {
                assoc->client_udp_addr = InetAddress(session.target_host(), session.target_port());
            }

            session.set_state(Socks5Session::State::udp_associate);
            fire_state_change(Socks5Session::State::request, Socks5Session::State::udp_associate, "udp_associate_started");

            InetAddress local_addr = udp_sock->get_local_address();
            std::string bind_ip = local_addr.get_ip().empty() ? "0.0.0.0" : local_addr.get_ip();
            int bind_port = local_addr.get_port();
            send_reply(client_conn, ReplyCode::succeeded,
                       AddressType::ipv4, bind_ip, static_cast<uint16_t>(bind_port));

            if (config_.udp_idle_timeout_ms > 0 && runtime) {
                Connection *client_conn_raw = &*client_conn;
                assoc->idle_timer = runtime->schedule(
                    config_.udp_idle_timeout_ms,
                    [this, client_conn_raw]() {
                        LOG_INFO("socks5 server: UDP association idle timer expired for client {}", reinterpret_cast<uintptr_t>(client_conn_raw));
                        metrics_.idle_timeouts.fetch_add(1, std::memory_order_relaxed);
                        close_udp_association(client_conn_raw);
                    });
            }

            udp_associations_[&*client_conn] = std::move(assoc);
            udp_associations_per_client_[client_ip]++;
            metrics_.active_udp_associations.fetch_add(1, std::memory_order_relaxed);

            fire_accepted();

            LOG_INFO("socks5 server: UDP associate established, relay port {}, client_ip={}", bind_port, client_ip);

            while (true) {
                auto result = co_await ctx.read_awaiter(config_.idle_timeout_ms);
                if (result.status != coroutine::IoStatus::success) {
                    break;
                }
            }

            uint64_t udp_bytes_up = 0;
            uint64_t udp_bytes_down = 0;
            {
                auto assoc_it = udp_associations_.find(&*client_conn);
                if (assoc_it != udp_associations_.end() && assoc_it->second) {
                    udp_bytes_up = assoc_it->second->bytes_up;
                    udp_bytes_down = assoc_it->second->bytes_down;
                }
            }

            close_udp_association(client_conn);

            metrics_.active_udp_associations.fetch_sub(1, std::memory_order_relaxed);

            if (handler_) {
                handler_->on_session_closed(&session);
            }

            fire_completed(udp_bytes_up, udp_bytes_down, "udp_idle_closed");
            fire_state_change(Socks5Session::State::udp_associate, Socks5Session::State::closed, "udp_idle_closed");
            co_return;
        }

        default:
            send_reply(client_conn, ReplyCode::command_not_supported);
            fire_rejected("command_not_supported");
            ctx.close();
            co_return;
        }
    }

    coroutine::Task<void> Socks5Server::relay_pipe(coroutine::RuntimeView rv,
                                                    std::shared_ptr<Connection> src,
                                                    std::shared_ptr<Connection> dst,
                                                    std::function<void(const std::string &)> on_close,
                                                    std::atomic_bool *dst_alive,
                                                    std::atomic_uint64_t *byte_counter)
    {
        const uint32_t idle_ms = config_.idle_timeout_ms > 0 ? config_.idle_timeout_ms : 0;
        while (true) {
            auto result = co_await coroutine::async_read(rv, src, idle_ms);
            if (result.status != coroutine::IoStatus::success) {
                bool peer_shutdown = result.status == coroutine::IoStatus::connection_closed &&
                                     src && src->input_shutdown();
                if (peer_shutdown) {
                    if (dst_alive && dst_alive->load(std::memory_order_acquire) && dst) {
                        dst->shutdown_write();
                    }
                    co_return;
                }
                if (on_close) {
                    on_close(result.status == coroutine::IoStatus::timed_out
                             ? "idle_timeout" : "read_error");
                }
                co_return;
            }
            if (!dst_alive || !dst_alive->load(std::memory_order_acquire)) {
                co_return;
            }
            if (result.data.readable_bytes() > 0) {
                const auto nbytes = result.data.readable_bytes();
                dst->write_and_flush(result.data);
                if (byte_counter) {
                    byte_counter->fetch_add(nbytes, std::memory_order_relaxed);
                }
            }
        }
    }

    void Socks5Server::send_reply(Connection * conn, ReplyCode reply,
                                  AddressType atyp, const std::string & bind_addr,
                                  uint16_t bind_port)
    {
        auto buf = Socks5PacketParser::build_reply(reply, atyp, bind_addr, bind_port);
        conn->write_and_flush(buf);
    }

    void Socks5Server::send_reply(Connection & conn, ReplyCode reply,
                                  AddressType atyp, const std::string & bind_addr,
                                  uint16_t bind_port)
    {
        send_reply(&conn, reply, atyp, bind_addr, bind_port);
    }

    void Socks5Server::send_reply(const std::shared_ptr<Connection> &conn, ReplyCode reply,
                                  AddressType atyp, const std::string &bind_addr,
                                  uint16_t bind_port)
    {
        if (!conn) {
            return;
        }
        send_reply(*conn, reply, atyp, bind_addr, bind_port);
    }

    void Socks5Server::on_udp_datagram(Connection * conn)
    {
        auto *udp_conn = dynamic_cast< ::yuan::net::UdpConnection *>(conn);
        if (!udp_conn || !udp_conn->datagram_instance()) {
            return;
        }

        auto *endpoint = udp_conn->datagram_instance()->acceptor();
        if (!endpoint) {
            return;
        }

        UdpAssociation *assoc = nullptr;
        std::shared_ptr<Connection> client_conn;
        for (auto & [conn_key, assoc_ptr] : udp_associations_) {
            if (assoc_ptr && assoc_ptr->udp_endpoint == endpoint) {
                assoc = &*assoc_ptr;
                client_conn = assoc_ptr->client_conn;
                break;
            }
        }

        if (!assoc || !client_conn) {
            return;
        }

        auto buf = conn->get_input_byte_buffer();
        const auto source_addr = conn->get_remote_address();
        const bool from_client = assoc->client_udp_addr.get_port() == 0 || source_addr == assoc->client_udp_addr;

        if (from_client) {
            if (assoc->client_udp_addr.get_port() == 0) {
                assoc->client_udp_addr = source_addr;
            }

            auto udp_header = Socks5PacketParser::parse_udp_header(buf);
            if (!udp_header) {
                LOG_WARN("socks5 server: failed to parse UDP header from client datagram");
                return;
            }

            if (udp_header->fragment != 0) {
                LOG_WARN("socks5 server: fragmented UDP datagrams not supported");
                return;
            }

            size_t header_size = 4;
            switch (udp_header->atyp) {
            case AddressType::ipv4:
                header_size += 4 + 2;
                break;
            case AddressType::domain:
                header_size += 1 + std::strlen(udp_header->address) + 2;
                break;
            case AddressType::ipv6:
                header_size += 16 + 2;
                break;
            default:
                return;
            }

            ::yuan::buffer::ByteBuffer payload(buf.readable_bytes() > header_size ? buf.readable_bytes() - header_size : 0);
            auto span = buf.readable_span();
            if (span.size() > header_size) {
                payload.append(span.data() + header_size, span.size() - header_size);
            }

            forward_udp_to_target(assoc, *udp_header, payload);
            return;
        }

        if (buf.readable_bytes() == 0) {
            return;
        }

        forward_udp_to_client(assoc, source_addr, buf);
    }

    void Socks5Server::on_udp_datagram(Connection &conn)
    {
        on_udp_datagram(&conn);
    }

    void Socks5Server::forward_udp_to_target(UdpAssociation * assoc, const Socks5UdpHeader & header, const ::yuan::buffer::ByteBuffer & payload)
    {
        if (payload.readable_bytes() == 0) {
            return;
        }

        if (config_.max_datagram_size > 0 && payload.readable_bytes() > config_.max_datagram_size) {
            LOG_WARN("socks5 server: UDP datagram too large {} bytes, max {} bytes, dropping",
                     payload.readable_bytes(), config_.max_datagram_size);
            return;
        }

        InetAddress target_addr(header.address, header.port);
        if (target_addr.get_ip().empty()) {
            LOG_WARN("socks5 server: failed to resolve UDP target {}:{}", header.address, header.port);
            return;
        }

        if (!config_.allow_private_targets && is_private_ip(target_addr.get_ip())) {
            LOG_WARN("socks5 server: UDP SSRF blocked, target {} resolves to private IP {}",
                     header.address, target_addr.get_ip());
            metrics_.closes_by_ssrf.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        assoc->udp_acceptor->send_datagram(target_addr, payload);
        assoc->target_addr = target_addr;
        assoc->bytes_up += payload.readable_bytes();

        if (assoc->idle_timer && config_.udp_idle_timeout_ms > 0) {
            assoc->idle_timer.cancel();
            auto *runtime = listener_.runtime();
            if (runtime) {
                Connection *client_conn_raw = &*assoc->client_conn;
                assoc->idle_timer = runtime->schedule(
                    config_.udp_idle_timeout_ms,
                    [this, client_conn_raw]() {
                        LOG_INFO("socks5 server: UDP association idle timer expired for client {}", reinterpret_cast<uintptr_t>(client_conn_raw));
                        metrics_.idle_timeouts.fetch_add(1, std::memory_order_relaxed);
                        close_udp_association(client_conn_raw);
                    });
            }
        }

        LOG_DEBUG("socks5 server: UDP forwarded to {}:{}, {} bytes",
                  header.address, header.port, payload.readable_bytes());
    }

    void Socks5Server::forward_udp_to_client(UdpAssociation * assoc, const InetAddress & target_addr, const ::yuan::buffer::ByteBuffer & payload)
    {
        if (!assoc || payload.readable_bytes() == 0) {
            return;
        }

        if (config_.max_datagram_size > 0 && payload.readable_bytes() > config_.max_datagram_size) {
            LOG_WARN("socks5 server: UDP response datagram too large {} bytes, max {} bytes, dropping",
                     payload.readable_bytes(), config_.max_datagram_size);
            return;
        }

        std::string target_ip = target_addr.get_ip();
        uint16_t target_port = target_addr.get_port();

        AddressType atyp = AddressType::ipv4;
        if (target_ip.find(':') != std::string::npos) {
            atyp = AddressType::ipv6;
        }

        auto udp_header_buf = Socks5PacketParser::build_udp_header(atyp, target_ip, target_port);

        ::yuan::buffer::ByteBuffer datagram(udp_header_buf.readable_bytes() + payload.readable_bytes());
        auto header_span = udp_header_buf.readable_span();
        datagram.append(header_span.data(), header_span.size());
        auto payload_span = payload.readable_span();
        datagram.append(payload_span.data(), payload_span.size());

        assoc->udp_acceptor->send_datagram(assoc->client_udp_addr, datagram);
        assoc->bytes_down += payload.readable_bytes();

        if (assoc->idle_timer && config_.udp_idle_timeout_ms > 0) {
            assoc->idle_timer.cancel();
            auto *runtime = listener_.runtime();
            if (runtime) {
                Connection *client_conn_raw = &*assoc->client_conn;
                assoc->idle_timer = runtime->schedule(
                    config_.udp_idle_timeout_ms,
                    [this, client_conn_raw]() {
                        LOG_INFO("socks5 server: UDP association idle timer expired for client {}", reinterpret_cast<uintptr_t>(client_conn_raw));
                        metrics_.idle_timeouts.fetch_add(1, std::memory_order_relaxed);
                        close_udp_association(client_conn_raw);
                    });
            }
        }

        LOG_DEBUG("socks5 server: UDP forwarded to client {}:{}, {} bytes",
                  assoc->client_udp_addr.get_ip(), assoc->client_udp_addr.get_port(),
                  datagram.readable_bytes());
    }

    void Socks5Server::close_udp_association(Connection * client_conn)
    {
        auto it = udp_associations_.find(client_conn);
        if (it == udp_associations_.end()) {
            return;
        }

        auto &assoc = it->second;
        if (assoc) {
            if (assoc->idle_timer) {
                assoc->idle_timer.cancel();
            }
            if (assoc->udp_acceptor) {
                assoc->udp_acceptor->close();
            }
            if (!assoc->client_ip.empty()) {
                auto cit = udp_associations_per_client_.find(assoc->client_ip);
                if (cit != udp_associations_per_client_.end()) {
                    if (cit->second > 0) {
                        --cit->second;
                    }
                    if (cit->second == 0) {
                        udp_associations_per_client_.erase(cit);
                    }
                }
            }
        }

        for (auto cit = udp_conn_to_client_.begin(); cit != udp_conn_to_client_.end();) {
            if (cit->second == client_conn) {
                cit = udp_conn_to_client_.erase(cit);
            } else {
                ++cit;
            }
        }

        udp_associations_.erase(it);
        LOG_INFO("socks5 server: UDP association closed");
    }

    void Socks5Server::close_udp_association(const std::shared_ptr<Connection> &client_conn)
    {
        if (client_conn) {
            close_udp_association(&*client_conn);
        }
    }

    Socks5Server::UdpRelayHandler::UdpRelayHandler(Socks5Server & server)
        : server_(server)
    {
    }

    void Socks5Server::UdpRelayHandler::on_read(Connection &conn)
    {
        server_.on_udp_datagram(conn);
    }

    void Socks5Server::UdpRelayHandler::on_error(Connection &conn)
    {
        (void)conn;
    }
}
