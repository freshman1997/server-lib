#include "socks5_server.h"
#include "socks5_packet_parser.h"
#include "net/acceptor/datagram_acceptor.h"
#include "net/acceptor/acceptor_factory.h"
#include "net/connection/connection.h"
#include "net/connection/tcp_connection.h"
#include "net/connection/udp_connection.h"
#include "net/connection/stream_transport.h"
#include "net/runtime/network_runtime.h"
#include "coroutine/connect_awaitable.h"
#include "coroutine/stream_io_awaitable.h"
#include "coroutine/io_result.h"
#include "net/socket/socket.h"
#include "net/socket/inet_address.h"
#include "logger.h"

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
                    assoc->idle_timer->cancel();
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
        return init(port, *owned_runtime_);
    }

    bool Socks5Server::init(int port, NetworkRuntime & runtime)
    {
        listener_.close();

        if (ssl_module_) {
            listener_.set_ssl_module(ssl_module_);
        }

        if (!listener_.bind(port, runtime)) {
            LOG_ERROR("socks5 server: failed to bind on port {}", port);
            if (owned_runtime_)
                owned_runtime_.reset();
            return false;
        }

        listener_.set_connection_handler([this](AsyncConnectionContext ctx) {
            auto task = handle_connection(std::move(ctx));
            task.resume();
            task.detach();
        });

        LOG_INFO("socks5 server: initialized on port {}", port);
        return true;
    }

    void Socks5Server::serve()
    {
        if (owned_runtime_) {
            auto task = listener_.run_async();
            task.resume();
            owned_runtime_->run();
        } else if (listener_.runtime()) {
            auto task = listener_.run_async();
            task.resume();
        }
    }

    void Socks5Server::stop()
    {
        if (owned_runtime_) {
            owned_runtime_->stop();
        }
    }

    coroutine::Task<void> Socks5Server::handle_connection(AsyncConnectionContext ctx)
    {
        Connection *client_conn = ctx.native_handle();
        Socks5Session session(client_conn);

        auto read_result = co_await ctx.read_async();
        if (read_result.status != coroutine::IoStatus::success) {
            ctx.close();
            co_return;
        }

        auto greeting = Socks5PacketParser::parse_greeting(read_result.data);
        if (!greeting) {
            ctx.close();
            co_return;
        }

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
            session.set_state(Socks5Session::State::auth);
        } else if (!config_.enable_auth && support_no_auth) {
            selected = AuthMethod::no_auth;
            session.set_state(Socks5Session::State::request);
        }

        auto method_reply = Socks5PacketParser::build_method_select_reply(selected);
        ctx.write_and_flush(method_reply);

        if (selected == AuthMethod::no_acceptable) {
            ctx.close();
            co_return;
        }

        if (selected == AuthMethod::username_password) {
            read_result = co_await ctx.read_async();
            if (read_result.status != coroutine::IoStatus::success) {
                ctx.close();
                co_return;
            }

            auto auth_result = Socks5PacketParser::parse_auth_request(read_result.data);
            if (!auth_result) {
                ctx.close();
                co_return;
            }

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
                ctx.close();
                co_return;
            }

            session.set_state(Socks5Session::State::request);
        }

        read_result = co_await ctx.read_async();
        if (read_result.status != coroutine::IoStatus::success) {
            ctx.close();
            co_return;
        }

        auto request = Socks5PacketParser::parse_request(read_result.data);
        if (!request) {
            send_reply(client_conn, ReplyCode::general_failure);
            ctx.close();
            co_return;
        }

        session.set_command(request->cmd);
        session.set_address_type(request->atyp);
        session.set_target_host(request->address);
        session.set_target_port(request->port);

        switch (request->cmd) {
        case Command::connect: {
            if (!config_.enable_connect) {
                send_reply(client_conn, ReplyCode::command_not_supported);
                ctx.close();
                co_return;
            }

            if (handler_) {
                if (!handler_->on_connect_request(&session, session.target_host(), session.target_port())) {
                    send_reply(client_conn, ReplyCode::connection_not_allowed);
                    ctx.close();
                    co_return;
                }
            }

            auto rv = ctx.runtime_view();
            auto connect_result = co_await coroutine::async_connect(
                rv, session.target_host(), session.target_port(), config_.connect_timeout_ms);

            if (connect_result.result != coroutine::ConnectResult::success || !connect_result.connection) {
                ReplyCode reply = ReplyCode::general_failure;
                if (connect_result.result == coroutine::ConnectResult::timed_out) {
                    reply = ReplyCode::ttl_expired;
                } else if (connect_result.result == coroutine::ConnectResult::connection_error ||
                           connect_result.result == coroutine::ConnectResult::connect_failed) {
                    reply = ReplyCode::connection_refused;
                }
                send_reply(client_conn, reply);
                ctx.close();
                co_return;
            }

            Connection *remote_conn = connect_result.connection;
            rv.register_connection(remote_conn, &relay_handler_);
            if (auto *stream = dynamic_cast<StreamTransport *>(remote_conn)) {
                if (auto *channel = stream->stream_channel()) {
                    rv.update_channel(channel);
                }
            }

            session.set_remote_connection(remote_conn);
            session.set_state(Socks5Session::State::established);

            const auto &remote_addr = remote_conn->get_remote_address();
            auto reply = Socks5PacketParser::build_reply(
                ReplyCode::succeeded, AddressType::ipv4,
                remote_addr.get_ip(), remote_addr.get_port());
            ctx.write_and_flush(reply);

            if (handler_) {
                handler_->on_session_opened(&session);
            }

            LOG_INFO("socks5 server: session established -> {}:{}",
                     session.target_host(), session.target_port());

            client_conn->set_connection_handler(&relay_handler_);

            auto t1 = relay_pipe(rv, client_conn, remote_conn);
            t1.resume();
            t1.detach();

            auto t2 = relay_pipe(rv, remote_conn, client_conn);
            co_await t2;

            if (handler_) {
                handler_->on_session_closed(&session);
            }
            co_return;
        }

        case Command::bind: {
            if (!config_.enable_bind) {
                send_reply(client_conn, ReplyCode::command_not_supported);
                ctx.close();
                co_return;
            }
            send_reply(client_conn, ReplyCode::command_not_supported);
            ctx.close();
            co_return;
        }

        case Command::udp_associate: {
            if (!config_.enable_udp_associate) {
                send_reply(client_conn, ReplyCode::command_not_supported);
                ctx.close();
                co_return;
            }

            if (handler_) {
                if (!handler_->on_connect_request(&session, session.target_host(), session.target_port())) {
                    send_reply(client_conn, ReplyCode::connection_not_allowed);
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
            DatagramAcceptor *udp_acceptor = create_datagram_acceptor(udp_sock, *runtime);
            if (!udp_acceptor->listen()) {
                delete udp_acceptor;
                LOG_ERROR("socks5 server: failed to listen on UDP socket for associate");
                send_reply(client_conn, ReplyCode::general_failure);
                ctx.close();
                co_return;
            }

            runtime->register_acceptor(udp_acceptor, &udp_relay_handler_, udp_acceptor->endpoint_channel());

            auto assoc = std::make_unique<UdpAssociation>();
            assoc->client_conn = client_conn;
            assoc->udp_acceptor = udp_acceptor;
            assoc->idle_timer = nullptr;

            if (session.target_host().empty() || session.target_port() == 0) {
                assoc->client_udp_addr = InetAddress();
            } else {
                assoc->client_udp_addr = InetAddress(session.target_host(), session.target_port());
            }

            session.set_state(Socks5Session::State::udp_associate);

            InetAddress *local_addr = udp_sock->get_address();
            std::string bind_ip = local_addr ? local_addr->get_ip() : "0.0.0.0";
            int bind_port = local_addr ? local_addr->get_port() : 0;
            send_reply(client_conn, ReplyCode::succeeded,
                       AddressType::ipv4, bind_ip, static_cast<uint16_t>(bind_port));

            udp_associations_[client_conn] = std::move(assoc);

            LOG_INFO("socks5 server: UDP associate established, relay port {}", bind_port);

            while (true) {
                auto result = co_await ctx.read_async(config_.idle_timeout_ms);
                if (result.status != coroutine::IoStatus::success) {
                    break;
                }
            }

            close_udp_association(client_conn);

            if (handler_) {
                handler_->on_session_closed(&session);
            }

            ctx.close();
            co_return;
        }

        default:
            send_reply(client_conn, ReplyCode::command_not_supported);
            ctx.close();
            co_return;
        }
    }

    coroutine::Task<void> Socks5Server::relay_pipe(coroutine::RuntimeView rv, Connection * src, Connection * dst)
    {
        while (true) {
            if (!src->is_connected()) {
                dst->close();
                co_return;
            }
            auto result = co_await coroutine::async_read(rv, src);
            if (result.status != coroutine::IoStatus::success) {
                dst->close();
                co_return;
            }
            if (result.data.readable_bytes() > 0) {
                dst->write_and_flush(result.data);
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

    void Socks5Server::on_udp_datagram(Connection * conn)
    {
        auto it = udp_conn_to_client_.find(conn);
        if (it == udp_conn_to_client_.end()) {
            for (auto & [
                            client_conn,
                            assoc
                        ] : udp_associations_) {
                if (assoc && assoc->udp_acceptor) {
                    udp_conn_to_client_[conn] = client_conn;
                    it = udp_conn_to_client_.find(conn);
                    break;
                }
            }

            if (it == udp_conn_to_client_.end()) {
                return;
            }
        }

        Connection *client_conn = it->second;
        auto assoc_it = udp_associations_.find(client_conn);
        if (assoc_it == udp_associations_.end() || !assoc_it->second) {
            return;
        }

        UdpAssociation *assoc = assoc_it->second.get();
        if (assoc->client_udp_addr.get_port() == 0) {
            assoc->client_udp_addr = conn->get_remote_address();
        }

        auto buf = conn->get_input_byte_buffer();
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
    }

    void Socks5Server::forward_udp_to_target(UdpAssociation * assoc, const Socks5UdpHeader & header, const ::yuan::buffer::ByteBuffer & payload)
    {
        if (payload.readable_bytes() == 0) {
            return;
        }

        InetAddress target_addr(header.address, header.port);
        assoc->udp_acceptor->send_datagram(target_addr, payload);

        LOG_DEBUG("socks5 server: UDP forwarded to {}:{}, {} bytes",
                  header.address, header.port, payload.readable_bytes());
    }

    void Socks5Server::forward_udp_to_client(UdpAssociation * assoc, const InetAddress & target_addr, const ::yuan::buffer::ByteBuffer & payload)
    {
        if (!assoc || payload.readable_bytes() == 0) {
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
                assoc->idle_timer->cancel();
            }
            if (assoc->udp_acceptor) {
                assoc->udp_acceptor->close();
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

    Socks5Server::UdpRelayHandler::UdpRelayHandler(Socks5Server & server)
        : server_(server)
    {
    }

    void Socks5Server::UdpRelayHandler::on_read(Connection * conn)
    {
        server_.on_udp_datagram(conn);
    }

    void Socks5Server::UdpRelayHandler::on_error(Connection * conn)
    {
        if (conn)
            conn->close();
    }
}
