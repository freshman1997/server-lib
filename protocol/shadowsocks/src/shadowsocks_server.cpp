#include "shadowsocks_server.h"

#include "buffer/byte_buffer.h"
#include "coroutine/connect_awaitable.h"
#include "coroutine/io_result.h"
#include "coroutine/stream_io_awaitable.h"
#include "endian/endian.hpp"
#include "logger.h"
#include "net/acceptor/acceptor_factory.h"
#include "net/acceptor/datagram_acceptor.h"
#include "net/acceptor/udp/udp_instance.h"
#include "net/connection/udp_connection.h"
#include "net/ip_policy.h"
#include "net/socket/inet_address.h"
#include "net/socket/socket.h"
#include "shadowsocks_crypto.h"
#include "shadowsocks_packet_codec.h"
#include "shadowsocks_session.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace yuan::net::shadowsocks
{
    namespace
    {
        bool validate_port(int port)
        {
            return port > 0 && port <= static_cast<int>(std::numeric_limits<uint16_t>::max());
        }

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
            std::transform(value.begin(), value.end(), value.begin(),
                           [](unsigned char c) {
                               return static_cast<char>(std::tolower(c));
                           });
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
            const bool port_ok = port_rule == "*" ||
                                 (!port_rule.empty() && static_cast<uint16_t>(std::stoi(port_rule)) == port);
            return host_ok && port_ok;
        }

        std::optional<CipherMethod> method_from_config(const ShadowsocksServerConfig &config)
        {
            return parse_method(config.method);
        }
    }

    ShadowsocksServer::UdpRelayHandler::UdpRelayHandler(ShadowsocksServer &server)
        : server_(server)
    {
    }

    void ShadowsocksServer::UdpRelayHandler::on_connected(Connection &conn)
    {
        (void)conn;
    }

    void ShadowsocksServer::UdpRelayHandler::on_read(Connection &conn)
    {
        server_.on_udp_datagram(&conn);
    }

    void ShadowsocksServer::UdpRelayHandler::on_write(Connection &conn)
    {
        (void)conn;
    }

    void ShadowsocksServer::UdpRelayHandler::on_error(Connection &conn)
    {
        auto it = server_.udp_conn_to_client_.find(&conn);
        if (it != server_.udp_conn_to_client_.end()) {
            server_.close_udp_association(it->second);
        }
    }

    void ShadowsocksServer::UdpRelayHandler::on_close(Connection &conn)
    {
        auto it = server_.udp_conn_to_client_.find(&conn);
        if (it != server_.udp_conn_to_client_.end()) {
            server_.close_udp_association(it->second);
        }
    }

    ShadowsocksServer::ShadowsocksServer()
        : udp_relay_handler_(*this)
    {
    }

    ShadowsocksServer::ShadowsocksServer(const ShadowsocksServerConfig &config)
        : udp_relay_handler_(*this), config_(config)
    {
    }

    ShadowsocksServer::~ShadowsocksServer()
    {
        stop();
    }

    bool ShadowsocksServer::validate_config()
    {
        if (!validate_port(config_.port)) {
            LOG_ERROR("shadowsocks server: invalid port {}", config_.port);
            return false;
        }
        if (config_.password.empty()) {
            LOG_ERROR("shadowsocks server: password must not be empty");
            return false;
        }
        if (!method_from_config(config_).has_value()) {
            LOG_ERROR("shadowsocks server: unsupported method {}", config_.method);
            return false;
        }
        if (!config_.enable_tcp && !config_.enable_udp) {
            LOG_ERROR("shadowsocks server: both tcp and udp are disabled");
            return false;
        }

        if (config_.connect_timeout_ms == 0) {
            config_.connect_timeout_ms = 10000;
        }
        if (config_.idle_timeout_ms == 0) {
            config_.idle_timeout_ms = 300000;
        }
        if (config_.udp_idle_timeout_ms == 0) {
            config_.udp_idle_timeout_ms = 300000;
        }
        if (config_.max_connections == 0) {
            config_.max_connections = 8192;
        }
        if (config_.max_datagram_size == 0) {
            config_.max_datagram_size = 65535;
        }
        return true;
    }

    bool ShadowsocksServer::init(int port)
    {
        return init(config_.listen_host, port);
    }

    bool ShadowsocksServer::init(int port, NetworkRuntime &runtime)
    {
        return init(config_.listen_host, port, runtime);
    }

    bool ShadowsocksServer::init(const std::string &host, int port)
    {
        owned_runtime_ = std::make_unique<NetworkRuntime>();
        return init(host, port, *owned_runtime_);
    }

    bool ShadowsocksServer::init(const std::string &host, int port, NetworkRuntime &runtime)
    {
        config_.listen_host = host;
        config_.port = port;

        if (!validate_config()) {
            return false;
        }

        method_ = method_from_config(config_);
        if (!method_.has_value()) {
            return false;
        }

        if (!ShadowsocksCrypto::derive_master_key(config_.password, *method_, master_key_)) {
            LOG_ERROR("shadowsocks server: failed to derive master key");
            return false;
        }

        return listener_.bind(host, static_cast<uint16_t>(port), runtime);
    }

    void ShadowsocksServer::serve()
    {
        if (listener_.runtime() == nullptr) {
            return;
        }

        stop_requested_.store(false, std::memory_order_relaxed);

        listener_.set_connection_handler(
            [this](AsyncConnectionContext ctx)->coroutine::Task<void> {
                co_await handle_connection(std::move(ctx));
            });

        if (config_.enable_udp && !udp_acceptor_) {
            auto bind_host = config_.listen_host;
            if (bind_host.empty()) {
                bind_host = "0.0.0.0";
            }

            auto udp_sock = std::make_unique<Socket>(bind_host, config_.port, true);
            udp_sock->set_none_block(true);
            udp_sock->set_reuse(true);
            if (!udp_sock->valid() || !udp_sock->bind()) {
                LOG_ERROR("shadowsocks server: failed to bind UDP {}:{}", bind_host, config_.port);
            } else {
                auto *rt = listener_.runtime();
                udp_acceptor_.reset(create_datagram_acceptor(udp_sock.release(), *rt));
                if (!udp_acceptor_ || !udp_acceptor_->listen()) {
                    LOG_ERROR("shadowsocks server: failed to listen UDP {}:{}", bind_host, config_.port);
                    udp_acceptor_.reset();
                } else {
                    rt->register_acceptor(udp_acceptor_, make_non_owning_handler(&udp_relay_handler_), udp_acceptor_->endpoint_channel());
                    LOG_INFO("shadowsocks server: UDP relay enabled on {}:{}", bind_host, config_.port);
                }
            }
        }

        auto task = listener_.run_async();
        task.resume();
        task.detach();

        if (owned_runtime_) {
            owned_runtime_->run();
        }
    }

    void ShadowsocksServer::stop()
    {
        stop_requested_.store(true, std::memory_order_relaxed);
        listener_.close();

        for (auto &entry : udp_associations_) {
            (void)entry;
        }
        if (udp_acceptor_) {
            udp_acceptor_->close();
            udp_acceptor_.reset();
        }
        udp_conn_to_client_.clear();
        udp_associations_.clear();

        if (owned_runtime_) {
            owned_runtime_->stop();
        }
    }

    void ShadowsocksServer::set_handler(ShadowsocksHandler *handler)
    {
        handler_ = handler;
    }

    const ShadowsocksServerConfig &ShadowsocksServer::config() const
    {
        return config_;
    }

    NetworkRuntime *ShadowsocksServer::runtime() const noexcept
    {
        return listener_.runtime();
    }

    bool ShadowsocksServer::target_allowed(const std::string &host, uint16_t port) const
    {
        for (const auto &rule : config_.deny_targets) {
            if (!rule.empty() && match_rule(rule, host, port)) {
                return false;
            }
        }
        if (config_.allow_targets.empty()) {
            return true;
        }
        for (const auto &rule : config_.allow_targets) {
            if (!rule.empty() && match_rule(rule, host, port)) {
                return true;
            }
        }
        return false;
    }

    bool ShadowsocksServer::parse_first_payload(ShadowsocksSession &session,
                                                const std::vector<uint8_t> &payload,
                                                ::yuan::buffer::ByteBuffer &out_initial_upstream_data)
    {
        std::size_t consumed = 0;
        auto target = ShadowsocksPacketCodec::parse_target_address(payload.data(), payload.size(), consumed);
        if (!target.has_value() || consumed > payload.size()) {
            return false;
        }

        session.set_target(*target);
        session.set_request_target_parsed(true);

        const std::size_t remain = payload.size() - consumed;
        if (remain > 0) {
            out_initial_upstream_data.append(reinterpret_cast<const char *>(payload.data() + consumed), remain);
        }
        return true;
    }

    coroutine::Task<void> ShadowsocksServer::relay_pipe(coroutine::RuntimeView rv,
                                                         std::shared_ptr<Connection> src,
                                                         std::shared_ptr<Connection> dst,
                                                         CipherMethod method,
                                                         const std::vector<uint8_t> &subkey,
                                                         std::vector<uint8_t> &send_nonce,
                                                         std::function<void()> on_close)
    {
        while (!stop_requested_.load(std::memory_order_relaxed)) {
            auto result = co_await coroutine::async_read(rv, src, config_.idle_timeout_ms);
            if (result.status != coroutine::IoStatus::success) {
                if (on_close) {
                    on_close();
                }
                co_return;
            }

            auto span = result.data.readable_span();
            std::size_t offset = 0;
            while (offset < span.size()) {
                const std::size_t remain = span.size() - offset;
                const std::size_t chunk = std::min<std::size_t>(remain, 0x3FFF);

                ::yuan::buffer::ByteBuffer encrypted(chunk + 64);
                if (!ShadowsocksPacketCodec::append_tcp_chunk(encrypted,
                                                              method,
                                                              subkey,
                                                              send_nonce,
                                                              reinterpret_cast<const uint8_t *>(span.data() + offset),
                                                              chunk)) {
                    if (on_close) {
                        on_close();
                    }
                    co_return;
                }

                dst->write_and_flush(encrypted);
                offset += chunk;
            }
        }

        co_return;
    }

    void ShadowsocksServer::close_udp_association(Connection *client_conn)
    {
        if (client_conn == nullptr) {
            return;
        }

        auto it = udp_associations_.find(client_conn);
        if (it == udp_associations_.end()) {
            return;
        }

        if (it->second && it->second->client_conn) {
            it->second->client_conn->close();
        }

        for (auto map_it = udp_conn_to_client_.begin(); map_it != udp_conn_to_client_.end();) {
            if (map_it->second == client_conn) {
                map_it = udp_conn_to_client_.erase(map_it);
            } else {
                ++map_it;
            }
        }

        udp_associations_.erase(it);
    }

    void ShadowsocksServer::on_udp_datagram(Connection *conn)
    {
        auto *udp_conn = dynamic_cast<UdpConnection *>(conn);
        if (!udp_conn || !udp_conn->datagram_instance()) {
            return;
        }

        auto *endpoint = udp_conn->datagram_instance()->acceptor();
        if (!endpoint) {
            return;
        }

        if (!udp_acceptor_ || endpoint != udp_acceptor_.get()) {
            return;
        }

        auto method_opt = method_;
        if (!method_opt.has_value()) {
            return;
        }
        const auto method = *method_opt;
        const auto &spec = method_spec(method);

        auto packet = conn->get_input_byte_buffer();
        auto span = packet.readable_span();
        if (span.empty()) {
            return;
        }

        auto client_it = udp_conn_to_client_.find(conn);
        if (client_it != udp_conn_to_client_.end()) {
            auto assoc_it = udp_associations_.find(client_it->second);
            if (assoc_it == udp_associations_.end()) {
                return;
            }
            forward_udp_to_client(assoc_it->second.get(), conn->get_remote_address(), packet);
            return;
        }

        auto assoc_it = udp_associations_.find(conn);
        if (assoc_it == udp_associations_.end()) {
            auto assoc = std::make_unique<UdpAssociation>();
            assoc->client_conn = conn->shared_from_this();
            assoc->client_ip = conn->get_remote_address().get_ip();
            assoc->bound_client_addr = std::make_unique<InetAddress>(conn->get_remote_address());
            udp_associations_[conn] = std::move(assoc);
            assoc_it = udp_associations_.find(conn);
        }

        UdpAssociation *assoc = assoc_it->second.get();
        if (!assoc) {
            return;
        }

        if (span.size() < spec.salt_size + spec.tag_size) {
            return;
        }

        const uint8_t *salt = reinterpret_cast<const uint8_t *>(span.data());
        auto decoded = ShadowsocksPacketCodec::parse_udp_packet(
            reinterpret_cast<const uint8_t *>(span.data()),
            span.size(),
            method,
            master_key_);
        if (!decoded.complete || decoded.malformed) {
            return;
        }

        ::yuan::buffer::ByteBuffer plain_buf(decoded.plaintext.size());
        plain_buf.append(reinterpret_cast<const char *>(decoded.plaintext.data()), decoded.plaintext.size());
        forward_udp_to_target(assoc, plain_buf);
    }

    void ShadowsocksServer::forward_udp_to_target(UdpAssociation *assoc, const ::yuan::buffer::ByteBuffer &plain_packet)
    {
        if (!assoc || !udp_acceptor_) {
            return;
        }

        std::size_t consumed = 0;
        auto target = ShadowsocksPacketCodec::parse_target_address(plain_packet, consumed);
        if (!target.has_value()) {
            return;
        }

        if (!target_allowed(target->host, target->port)) {
            return;
        }

        InetAddress target_addr(target->host, target->port);
        if (target_addr.get_ip().empty()) {
            return;
        }

        if (!config_.allow_private_targets && is_private_ip(target_addr.get_ip())) {
            return;
        }

        auto span = plain_packet.readable_span();
        if (consumed >= span.size()) {
            return;
        }

        ::yuan::buffer::ByteBuffer payload(span.size() - consumed);
        payload.append(span.data() + consumed, span.size() - consumed);
        if (udp_acceptor_->send_datagram(target_addr, payload) < 0) {
            return;
        }

        if (auto *instance = udp_acceptor_->get_udp_instance()) {
            auto pair = instance->on_recv(target_addr);
            if (pair.first && pair.second) {
                udp_conn_to_client_[pair.second.get()] = assoc->client_conn.get();
            }
        }
    }

    void ShadowsocksServer::forward_udp_to_client(UdpAssociation *assoc,
                                                  const InetAddress &source_addr,
                                                  const ::yuan::buffer::ByteBuffer &payload)
    {
        if (!assoc || !assoc->client_conn || !method_.has_value()) {
            return;
        }

        const auto method = *method_;
        const auto &spec = method_spec(method);

        TargetAddress target;
        target.host = source_addr.get_ip();
        target.port = static_cast<uint16_t>(source_addr.get_port());
        target.atyp = source_addr.is_ipv6() ? AddressType::ipv6 : AddressType::ipv4;

        ::yuan::buffer::ByteBuffer plain(payload.readable_bytes() + 64);
        if (!ShadowsocksPacketCodec::append_target_address(plain, target)) {
            return;
        }
        plain.append(payload.readable_span());

        auto span = plain.readable_span();
        ::yuan::buffer::ByteBuffer out(span.size() + spec.salt_size + spec.tag_size);
        if (!ShadowsocksPacketCodec::append_udp_packet(out,
                                                       method,
                                                       master_key_,
                                                       reinterpret_cast<const uint8_t *>(span.data()),
                                                       span.size())) {
            return;
        }

        assoc->client_conn->write_and_flush(out);
    }

    coroutine::Task<void> ShadowsocksServer::handle_connection(AsyncConnectionContext ctx)
    {
        if (!config_.enable_tcp) {
            ctx.close();
            co_return;
        }

        auto method_opt = method_;
        if (!method_opt.has_value()) {
            ctx.close();
            co_return;
        }
        const auto method = *method_opt;
        const auto &spec = method_spec(method);

        ShadowsocksSession session(ctx.connection());
        session.set_state(ShadowsocksSession::State::reading_salt);

        ::yuan::buffer::ByteBuffer recv_buf;
        bool remote_connected = false;
        bool running = true;

        while (running && ctx.is_connected() && !stop_requested_.load(std::memory_order_relaxed)) {
            auto read_result = co_await ctx.read_awaiter(config_.idle_timeout_ms);
            if (read_result.status != coroutine::IoStatus::success) {
                break;
            }

            recv_buf.append(read_result.data);

            while (recv_buf.readable_bytes() > 0) {
                if (session.state() == ShadowsocksSession::State::reading_salt) {
                    if (recv_buf.readable_bytes() < spec.salt_size) {
                        break;
                    }

                    auto span = recv_buf.readable_span();
                    const uint8_t *salt = reinterpret_cast<const uint8_t *>(span.data());
                    if (!ShadowsocksCrypto::derive_subkey(master_key_,
                                                          method,
                                                          salt,
                                                          spec.salt_size,
                                                          session.mutable_recv_subkey())) {
                        running = false;
                        break;
                    }

                    std::vector<uint8_t> server_salt;
                    if (!ShadowsocksCrypto::random_bytes(spec.salt_size, server_salt)) {
                        running = false;
                        break;
                    }

                    if (!ShadowsocksCrypto::derive_subkey(master_key_,
                                                          method,
                                                          server_salt.data(),
                                                          server_salt.size(),
                                                          session.mutable_send_subkey())) {
                        running = false;
                        break;
                    }

                    session.mutable_recv_nonce().assign(spec.nonce_size, 0);
                    session.mutable_send_nonce().assign(spec.nonce_size, 0);
                    recv_buf.consume(spec.salt_size);
                    session.set_state(ShadowsocksSession::State::reading_request);

                    ::yuan::buffer::ByteBuffer salt_out(server_salt.size());
                    salt_out.append(reinterpret_cast<const char *>(server_salt.data()), server_salt.size());
                    session.client_connection()->write_and_flush(salt_out);
                    continue;
                }

                if (session.state() != ShadowsocksSession::State::reading_request &&
                    session.state() != ShadowsocksSession::State::established) {
                    running = false;
                    break;
                }

                auto span = recv_buf.readable_span();
                auto parsed = ShadowsocksPacketCodec::try_parse_tcp_chunk(
                    reinterpret_cast<const uint8_t *>(span.data()),
                    span.size(),
                    method,
                    session.recv_subkey(),
                    session.mutable_recv_nonce());

                if (!parsed.complete) {
                    if (parsed.malformed) {
                        running = false;
                    }
                    break;
                }

                recv_buf.consume(parsed.consumed);

                if (!session.request_target_parsed()) {
                    ::yuan::buffer::ByteBuffer first_payload(parsed.plaintext.size());
                    if (!parse_first_payload(session, parsed.plaintext, first_payload)) {
                        running = false;
                        break;
                    }

                    if (!target_allowed(session.target().host, session.target().port)) {
                        running = false;
                        break;
                    }

                    InetAddress target_addr(session.target().host, session.target().port);
                    if (target_addr.get_ip().empty()) {
                        running = false;
                        break;
                    }

                    if (!config_.allow_private_targets && is_private_ip(target_addr.get_ip())) {
                        running = false;
                        break;
                    }

                    if (handler_) {
                        if (!handler_->on_connect_request(ctx.get_remote_address().to_address_key(), session.target())) {
                            running = false;
                            break;
                        }
                    }

                    auto connect_result = co_await coroutine::async_connect(
                        ctx.runtime_view(),
                        target_addr.get_ip(),
                        static_cast<uint16_t>(target_addr.get_port()),
                        config_.connect_timeout_ms);
                    if (connect_result.result != coroutine::ConnectResult::success || !connect_result.connection) {
                        running = false;
                        break;
                    }

                    session.set_remote_connection(connect_result.connection);
                    session.set_state(ShadowsocksSession::State::established);
                    remote_connected = true;

                    if (handler_) {
                        handler_->on_session_opened(ctx.get_remote_address().to_address_key(), session.target());
                    }

                    if (first_payload.readable_bytes() > 0) {
                        session.remote_connection()->write_and_flush(first_payload);
                    }

                    auto *loop = ctx.runtime_view().event_loop();
                    if (!loop) {
                        running = false;
                        break;
                    }

                    auto close_all = [client = session.client_connection(), remote = session.remote_connection()]() {
                        if (client) {
                            client->close();
                        }
                        if (remote) {
                            remote->close();
                        }
                    };

                    auto relay_task = relay_pipe(ctx.runtime_view(),
                                                 session.remote_connection(),
                                                 session.client_connection(),
                                                 method,
                                                 session.send_subkey(),
                                                 session.mutable_send_nonce(),
                                                 close_all);
                    relay_task.resume();
                    relay_task.detach();
                    continue;
                }

                if (remote_connected && !parsed.plaintext.empty()) {
                    ::yuan::buffer::ByteBuffer plain(parsed.plaintext.size());
                    plain.append(reinterpret_cast<const char *>(parsed.plaintext.data()), parsed.plaintext.size());
                    session.remote_connection()->write_and_flush(plain);
                }
            }

            if (recv_buf.readable_bytes() == 0) {
                recv_buf.clear();
            } else {
                recv_buf.compact();
            }
        }

        if (handler_ && session.request_target_parsed()) {
            handler_->on_session_closed(ctx.get_remote_address().to_address_key(), session.target(), "closed");
        }

        if (session.remote_connection()) {
            session.remote_connection()->close();
        }
        ctx.close();
        co_return;
    }
}
