#include "ssh_cli_forward_runtime.h"

#include "protocol/ssh_message_codec.h"

#include <array>
#include <iostream>

#ifdef _WIN32
#include <winsock2.h>
#else
#include <arpa/inet.h>
#include <sys/socket.h>
#endif

namespace yuan::release_ssh::client
{
    SshCliForwardRuntime::~SshCliForwardRuntime()
    {
        cleanup();
    }

    bool SshCliForwardRuntime::configure(const CliArgs &args, std::ostream &err)
    {
        for (const auto &raw : args.local_forwards) {
            auto spec = parse_local_forward_spec(raw);
            if (!spec) {
                err << "invalid -L spec: " << raw << '\n';
                return false;
            }
            local_forward_specs.push_back(*spec);

            SocketHandle listen_fd = kInvalidSocket;
            if (!create_listen_socket(spec->bind_addr, spec->bind_port, listen_fd)) {
                err << "failed to create -L listener on "
                    << spec->bind_addr << ':' << spec->bind_port << '\n';
                return false;
            }
            const std::string key = spec->bind_addr + ":" + std::to_string(spec->bind_port);
            local_forward_listeners[key] = listen_fd;
        }

        for (const auto &raw : args.dynamic_forwards) {
            auto spec = parse_dynamic_forward_spec(raw);
            if (!spec) {
                err << "invalid -D spec: " << raw << '\n';
                return false;
            }
            dynamic_forward_specs.push_back(*spec);

            SocketHandle listen_fd = kInvalidSocket;
            if (!create_listen_socket(spec->bind_addr, spec->bind_port, listen_fd)) {
                err << "failed to create -D listener on "
                    << spec->bind_addr << ':' << spec->bind_port << '\n';
                return false;
            }
            const std::string key = spec->bind_addr + ":" + std::to_string(spec->bind_port);
            dynamic_forward_listeners[key] = listen_fd;
        }

        for (const auto &raw : args.remote_forwards) {
            auto spec = parse_remote_forward_spec(raw);
            if (!spec) {
                err << "invalid -R spec: " << raw << '\n';
                return false;
            }
            pending_remote_forward_specs.push_back(*spec);
        }

        return true;
    }

    void SshCliForwardRuntime::cleanup()
    {
        for (auto &it : local_forward_listeners) {
            close_socket_handle(it.second);
        }
        for (auto &it : dynamic_forward_listeners) {
            close_socket_handle(it.second);
        }
        for (auto &it : pending_local_open_socket) {
            close_socket_handle(it.second);
        }
        for (auto &it : pending_dynamic_open_socket) {
            close_socket_handle(it.second);
        }
        for (auto &it : pending_socks_clients) {
            close_socket_handle(it.second.socket);
        }
        for (auto &it : forward_local_to_socket) {
            close_socket_handle(it.second);
        }
    }

    void SshCliForwardRuntime::close_channel(uint32_t local_id)
    {
        auto sock_it = forward_local_to_socket.find(local_id);
        if (sock_it != forward_local_to_socket.end()) {
            close_socket_handle(sock_it->second);
            forward_local_to_socket.erase(sock_it);
        }
        auto remote_it = forward_local_to_remote.find(local_id);
        if (remote_it != forward_local_to_remote.end()) {
            forward_remote_to_local.erase(remote_it->second);
            forward_local_to_remote.erase(remote_it);
        }
    }

    bool SshCliForwardRuntime::has_forwarding() const
    {
        return !local_forward_specs.empty() ||
               !dynamic_forward_specs.empty() ||
               !pending_remote_forward_specs.empty();
    }

    bool SshCliForwardRuntime::has_activity() const
    {
        return !pending_local_open_socket.empty() ||
               !pending_dynamic_open_socket.empty() ||
               !pending_socks_clients.empty() ||
               !forward_local_to_socket.empty();
    }

    bool SshCliForwardRuntime::send_socks_reply(SocketHandle sock, uint8_t rep_code)
    {
        const std::array<uint8_t, 10> reply = {
            0x05,
            rep_code,
            0x00,
            0x01,
            0x00,
            0x00,
            0x00,
            0x00,
            0x00,
            0x00
        };
        return send_all(sock, reply.data(), reply.size());
    }

    bool SshCliForwardRuntime::pump_local_forward_accepts(SocketHandle ssh_fd,
                                                          yuan::net::ssh::SshTransport &transport,
                                                          bool auth_ok,
                                                          const std::function<void(const std::string &)> &debug,
                                                          std::ostream &err)
    {
        using namespace yuan::net::ssh;

        if (!auth_ok || local_forward_specs.empty()) {
            return true;
        }

        if (!pending_local_open_socket.empty() || !pending_dynamic_open_socket.empty()) {
            return true;
        }

        for (const auto &spec : local_forward_specs) {
            const std::string key = spec.bind_addr + ":" + std::to_string(spec.bind_port);
            auto listener_it = local_forward_listeners.find(key);
            if (listener_it == local_forward_listeners.end()) {
                continue;
            }

            std::string origin_host;
            uint16_t origin_port = 0;
            bool had_fatal_error = false;
            SocketHandle accepted_fd = kInvalidSocket;
            const bool accepted = accept_pending_client(
                listener_it->second,
                accepted_fd,
                origin_host,
                origin_port,
                had_fatal_error);

            if (had_fatal_error) {
                err << "local forward listener failure on " << key << '\n';
                return false;
            }
            if (!accepted) {
                continue;
            }

            SshChannelOpenMessage open_msg;
            const uint32_t local_forward_id = next_forward_local_channel_id++;
            open_msg.channel_type = SSH_CHANNEL_DIRECT_TCPIP;
            open_msg.sender_channel = local_forward_id;
            open_msg.initial_window_size = SSH_DEFAULT_WINDOW_SIZE;
            open_msg.maximum_packet_size = SSH_DEFAULT_MAX_PACKET_SIZE;

            yuan::buffer::ByteBuffer open_data;
            SshMessageCodec::write_string(open_data, spec.target_host);
            SshMessageCodec::write_uint32(open_data, spec.target_port);
            SshMessageCodec::write_string(open_data, origin_host);
            SshMessageCodec::write_uint32(open_data, origin_port);
            open_msg.type_specific_data.assign(
                reinterpret_cast<const uint8_t *>(open_data.read_ptr()),
                reinterpret_cast<const uint8_t *>(open_data.read_ptr()) + open_data.readable_bytes());

            if (!send_packet(ssh_fd, transport, SshMessageCodec::encode_channel_open(open_msg))) {
                close_socket_handle(accepted_fd);
                return false;
            }
            debug("local-forward open sent local=" + std::to_string(local_forward_id));
            pending_local_open_socket[local_forward_id] = accepted_fd;
            break;
        }
        return true;
    }

    void SshCliForwardRuntime::close_socks_client(SocketHandle sock)
    {
        auto it = pending_socks_clients.find(sock);
        if (it != pending_socks_clients.end()) {
            close_socket_handle(it->second.socket);
            pending_socks_clients.erase(it);
            return;
        }
        close_socket_handle(sock);
    }

    bool SshCliForwardRuntime::pump_dynamic_forward_accepts(bool auth_ok,
                                                            const std::function<void(const std::string &)> &debug,
                                                            std::ostream &err)
    {
        if (!auth_ok || dynamic_forward_specs.empty()) {
            return true;
        }

        if (!pending_local_open_socket.empty() || !pending_dynamic_open_socket.empty()) {
            return true;
        }

        for (const auto &spec : dynamic_forward_specs) {
            const std::string key = spec.bind_addr + ":" + std::to_string(spec.bind_port);
            auto listener_it = dynamic_forward_listeners.find(key);
            if (listener_it == dynamic_forward_listeners.end()) {
                continue;
            }

            std::string origin_host;
            uint16_t origin_port = 0;
            bool had_fatal_error = false;
            SocketHandle accepted_fd = kInvalidSocket;
            const bool accepted = accept_pending_client(
                listener_it->second,
                accepted_fd,
                origin_host,
                origin_port,
                had_fatal_error);
            if (had_fatal_error) {
                err << "dynamic forward listener failure on " << key << '\n';
                return false;
            }
            if (!accepted) {
                continue;
            }

            PendingSocksClient client;
            client.socket = accepted_fd;
            client.origin_host = origin_host;
            client.origin_port = origin_port;
            pending_socks_clients[accepted_fd] = std::move(client);
            debug("dynamic-forward accepted client fd=" + std::to_string(static_cast<int>(accepted_fd)) +
                  " origin=" + origin_host + ":" + std::to_string(origin_port));
            break;
        }
        return true;
    }

    bool SshCliForwardRuntime::pump_dynamic_socks_handshake(SocketHandle ssh_fd,
                                                            yuan::net::ssh::SshTransport &transport,
                                                            const std::function<void(const std::string &)> &debug)
    {
        using namespace yuan::net::ssh;

        if (pending_socks_clients.empty()) {
            return true;
        }

        std::vector<SocketHandle> sockets;
        sockets.reserve(pending_socks_clients.size());
        for (const auto &it : pending_socks_clients) {
            sockets.push_back(it.first);
        }

        for (SocketHandle sock : sockets) {
            auto client_it = pending_socks_clients.find(sock);
            if (client_it == pending_socks_clients.end()) {
                continue;
            }

            if (!socket_read_ready(sock)) {
                continue;
            }

            std::array<uint8_t, 4096> chunk{};
#ifdef _WIN32
            const int n = recv(sock, reinterpret_cast<char *>(chunk.data()), static_cast<int>(chunk.size()), 0);
#else
            const ssize_t n = recv(sock, chunk.data(), chunk.size(), 0);
#endif
            if (n == 0) {
                close_socks_client(sock);
                continue;
            }
            if (n < 0) {
                if (socket_would_block_last_error()) {
                    continue;
                }
                close_socks_client(sock);
                continue;
            }

            auto &client = client_it->second;
            client.recv_buf.insert(client.recv_buf.end(), chunk.begin(), chunk.begin() + n);

            while (!client.recv_buf.empty()) {
                if (!client.method_negotiated &&
                    client.recv_buf.size() >= 2 &&
                    client.recv_buf[0] == 0x05) {
                    const size_t nmethods = client.recv_buf[1];
                    const size_t method_len = 2 + nmethods;
                    if (client.recv_buf.size() >= method_len) {
                        bool supports_no_auth = false;
                        for (size_t i = 0; i < nmethods; ++i) {
                            if (client.recv_buf[2 + i] == 0x00) {
                                supports_no_auth = true;
                                break;
                            }
                        }

                        const std::array<uint8_t, 2> method_reply = {
                            0x05,
                            static_cast<uint8_t>(supports_no_auth ? 0x00 : 0xFF)
                        };
                        if (!send_all(client.socket, method_reply.data(), method_reply.size())) {
                            close_socks_client(sock);
                            break;
                        }
                        client.recv_buf.erase(client.recv_buf.begin(), client.recv_buf.begin() + static_cast<std::ptrdiff_t>(method_len));
                        if (!supports_no_auth) {
                            close_socks_client(sock);
                            break;
                        }
                        client.method_negotiated = true;
                        debug("dynamic-forward socks method negotiation ok fd=" +
                              std::to_string(static_cast<int>(client.socket)));
                        continue;
                    }
                }

                if (client.recv_buf.size() < 4) {
                    break;
                }

                const uint8_t ver = client.recv_buf[0];
                const uint8_t cmd = client.recv_buf[1];
                const uint8_t rsv = client.recv_buf[2];
                const uint8_t atyp = client.recv_buf[3];
                if (ver != 0x05 || rsv != 0x00) {
                    (void)send_socks_reply(client.socket, 0x01);
                    close_socks_client(sock);
                    break;
                }
                if (cmd != 0x01) {
                    (void)send_socks_reply(client.socket, 0x07);
                    close_socks_client(sock);
                    break;
                }

                size_t offset = 4;
                std::string target_host;
                if (atyp == 0x01) {
                    if (client.recv_buf.size() < offset + 4 + 2) {
                        break;
                    }
                    target_host = std::to_string(client.recv_buf[offset]) + "." +
                                  std::to_string(client.recv_buf[offset + 1]) + "." +
                                  std::to_string(client.recv_buf[offset + 2]) + "." +
                                  std::to_string(client.recv_buf[offset + 3]);
                    offset += 4;
                } else if (atyp == 0x03) {
                    if (client.recv_buf.size() < offset + 1) {
                        break;
                    }
                    const size_t host_len = client.recv_buf[offset++];
                    if (client.recv_buf.size() < offset + host_len + 2) {
                        break;
                    }
                    target_host.assign(reinterpret_cast<const char *>(&client.recv_buf[offset]), host_len);
                    offset += host_len;
                } else if (atyp == 0x04) {
                    if (client.recv_buf.size() < offset + 16 + 2) {
                        break;
                    }
                    char host_text[INET6_ADDRSTRLEN] = {};
                    if (!inet_ntop(AF_INET6, client.recv_buf.data() + offset, host_text, sizeof(host_text))) {
                        (void)send_socks_reply(client.socket, 0x08);
                        close_socks_client(sock);
                        break;
                    }
                    target_host = host_text;
                    offset += 16;
                } else {
                    (void)send_socks_reply(client.socket, 0x08);
                    close_socks_client(sock);
                    break;
                }

                if (client.recv_buf.size() < offset + 2) {
                    break;
                }
                const uint16_t target_port =
                    static_cast<uint16_t>((static_cast<uint16_t>(client.recv_buf[offset]) << 8u) |
                                          static_cast<uint16_t>(client.recv_buf[offset + 1]));
                client.recv_buf.erase(client.recv_buf.begin(), client.recv_buf.begin() + static_cast<std::ptrdiff_t>(offset + 2));

                SshChannelOpenMessage open_msg;
                const uint32_t local_forward_id = next_forward_local_channel_id++;
                open_msg.channel_type = SSH_CHANNEL_DIRECT_TCPIP;
                open_msg.sender_channel = local_forward_id;
                open_msg.initial_window_size = SSH_DEFAULT_WINDOW_SIZE;
                open_msg.maximum_packet_size = SSH_DEFAULT_MAX_PACKET_SIZE;

                yuan::buffer::ByteBuffer open_data;
                SshMessageCodec::write_string(open_data, target_host);
                SshMessageCodec::write_uint32(open_data, target_port);
                SshMessageCodec::write_string(open_data, client.origin_host);
                SshMessageCodec::write_uint32(open_data, client.origin_port);
                open_msg.type_specific_data.assign(
                    reinterpret_cast<const uint8_t *>(open_data.read_ptr()),
                    reinterpret_cast<const uint8_t *>(open_data.read_ptr()) + open_data.readable_bytes());

                if (!send_packet(ssh_fd, transport, SshMessageCodec::encode_channel_open(open_msg))) {
                    close_socks_client(sock);
                    return false;
                }
                debug("dynamic-forward open sent local=" + std::to_string(local_forward_id) +
                      " target=" + target_host + ":" + std::to_string(target_port));

                pending_dynamic_open_socket[local_forward_id] = client.socket;
                pending_socks_clients.erase(client_it);
                break;
            }
        }

        return true;
    }

    bool SshCliForwardRuntime::pump_forward_target_reads(SocketHandle ssh_fd,
                                                         yuan::net::ssh::SshTransport &transport,
                                                         const std::function<void(const std::string &)> &debug)
    {
        using namespace yuan::net::ssh;

        std::vector<uint32_t> local_ids;
        local_ids.reserve(forward_local_to_socket.size());
        for (const auto &it : forward_local_to_socket) {
            local_ids.push_back(it.first);
        }

        std::array<uint8_t, 64 * 1024> buffer{};
        for (uint32_t local_id : local_ids) {
            auto sock_it = forward_local_to_socket.find(local_id);
            if (sock_it == forward_local_to_socket.end()) {
                continue;
            }
            if (!socket_read_ready(sock_it->second)) {
                continue;
            }

#ifdef _WIN32
            const int n = recv(sock_it->second,
                               reinterpret_cast<char *>(buffer.data()),
                               static_cast<int>(buffer.size()),
                               0);
#else
            const ssize_t n = recv(sock_it->second,
                                   buffer.data(),
                                   buffer.size(),
                                   0);
#endif
            if (n == 0) {
                auto remote_it = forward_local_to_remote.find(local_id);
                if (remote_it != forward_local_to_remote.end()) {
                    SshChannelEofMessage eof_msg;
                    eof_msg.recipient_channel = remote_it->second;
                    (void)send_packet(ssh_fd, transport, SshMessageCodec::encode_channel_eof(eof_msg));

                    SshChannelCloseMessage close_msg;
                    close_msg.recipient_channel = remote_it->second;
                    (void)send_packet(ssh_fd, transport, SshMessageCodec::encode_channel_close(close_msg));
                }
                close_channel(local_id);
                continue;
            }
            if (n < 0) {
                if (socket_would_block_last_error()) {
                    continue;
                }
                auto remote_it = forward_local_to_remote.find(local_id);
                if (remote_it != forward_local_to_remote.end()) {
                    SshChannelCloseMessage close_msg;
                    close_msg.recipient_channel = remote_it->second;
                    (void)send_packet(ssh_fd, transport, SshMessageCodec::encode_channel_close(close_msg));
                }
                close_channel(local_id);
                continue;
            }

            auto remote_it = forward_local_to_remote.find(local_id);
            if (remote_it == forward_local_to_remote.end()) {
                continue;
            }

            SshChannelDataMessage data_msg;
            data_msg.recipient_channel = remote_it->second;
            data_msg.data.assign(buffer.begin(), buffer.begin() + n);
            if (!send_packet(ssh_fd, transport, SshMessageCodec::encode_channel_data(data_msg))) {
                return false;
            }
            debug("local-forward send data bytes=" + std::to_string(static_cast<int>(n)) +
                  " local=" + std::to_string(local_id) +
                  " remote=" + std::to_string(remote_it->second));
        }
        return true;
    }

    bool SshCliForwardRuntime::send_remote_forward_requests(SocketHandle ssh_fd,
                                                            yuan::net::ssh::SshTransport &transport)
    {
        using namespace yuan::net::ssh;

        for (const auto &spec : pending_remote_forward_specs) {
            SshGlobalRequestMessage req;
            req.request_name = "tcpip-forward";
            req.want_reply = true;
            yuan::buffer::ByteBuffer data;
            SshMessageCodec::write_string(data, spec.bind_addr);
            SshMessageCodec::write_uint32(data, spec.bind_port);
            req.request_specific_data.assign(
                reinterpret_cast<const uint8_t *>(data.read_ptr()),
                reinterpret_cast<const uint8_t *>(data.read_ptr()) + data.readable_bytes());
            if (!send_packet(ssh_fd, transport, SshMessageCodec::encode_global_request(req))) {
                return false;
            }
        }
        return true;
    }

    void SshCliForwardRuntime::send_remote_forward_cancel_requests(SocketHandle ssh_fd,
                                                                   yuan::net::ssh::SshTransport &transport)
    {
        using namespace yuan::net::ssh;

        for (const auto &spec : remote_forward_specs) {
            SshGlobalRequestMessage req;
            req.request_name = "cancel-tcpip-forward";
            req.want_reply = false;
            yuan::buffer::ByteBuffer data;
            SshMessageCodec::write_string(data, spec.bind_addr);
            SshMessageCodec::write_uint32(data, spec.bind_port);
            req.request_specific_data.assign(
                reinterpret_cast<const uint8_t *>(data.read_ptr()),
                reinterpret_cast<const uint8_t *>(data.read_ptr()) + data.readable_bytes());
            (void)send_packet(ssh_fd, transport, SshMessageCodec::encode_global_request(req));
        }
    }

    bool SshCliForwardRuntime::handle_open_confirmation(const yuan::net::ssh::SshChannelOpenConfirmationMessage &conf,
                                                        const std::function<void(const std::string &)> &debug,
                                                        bool &handled)
    {
        handled = false;

        auto pending_it = pending_local_open_socket.find(conf.recipient_channel);
        if (pending_it != pending_local_open_socket.end()) {
            forward_local_to_socket[conf.recipient_channel] = pending_it->second;
            forward_local_to_remote[conf.recipient_channel] = conf.sender_channel;
            forward_remote_to_local[conf.sender_channel] = conf.recipient_channel;
            debug("local-forward open confirmed local=" + std::to_string(conf.recipient_channel) +
                  " remote=" + std::to_string(conf.sender_channel));
            pending_local_open_socket.erase(pending_it);
            handled = true;
            return true;
        }

        auto pending_dynamic_it = pending_dynamic_open_socket.find(conf.recipient_channel);
        if (pending_dynamic_it != pending_dynamic_open_socket.end()) {
            forward_local_to_socket[conf.recipient_channel] = pending_dynamic_it->second;
            forward_local_to_remote[conf.recipient_channel] = conf.sender_channel;
            forward_remote_to_local[conf.sender_channel] = conf.recipient_channel;
            debug("dynamic-forward open confirmed local=" + std::to_string(conf.recipient_channel) +
                  " remote=" + std::to_string(conf.sender_channel));
            (void)send_socks_reply(pending_dynamic_it->second, 0x00);
            pending_dynamic_open_socket.erase(pending_dynamic_it);
            handled = true;
            return true;
        }

        return true;
    }

    bool SshCliForwardRuntime::handle_open_failure(const yuan::net::ssh::SshChannelOpenFailureMessage &failure,
                                                   const std::function<void(const std::string &)> &debug,
                                                   bool &handled)
    {
        handled = false;

        auto pending_it = pending_local_open_socket.find(failure.recipient_channel);
        if (pending_it != pending_local_open_socket.end()) {
            close_socket_handle(pending_it->second);
            pending_local_open_socket.erase(pending_it);
            handled = true;
            return true;
        }

        auto pending_dynamic_it = pending_dynamic_open_socket.find(failure.recipient_channel);
        if (pending_dynamic_it != pending_dynamic_open_socket.end()) {
            debug("dynamic-forward open failed local=" + std::to_string(failure.recipient_channel) +
                  " reason=" + std::to_string(failure.reason_code));
            (void)send_socks_reply(pending_dynamic_it->second, 0x05);
            close_socket_handle(pending_dynamic_it->second);
            pending_dynamic_open_socket.erase(pending_dynamic_it);
            handled = true;
            return true;
        }

        return true;
    }

    bool SshCliForwardRuntime::handle_request_success()
    {
        if (!pending_remote_forward_specs.empty()) {
            remote_forward_specs.push_back(pending_remote_forward_specs.front());
            pending_remote_forward_specs.erase(pending_remote_forward_specs.begin());
        }
        return true;
    }

    bool SshCliForwardRuntime::handle_request_failure(std::ostream &err)
    {
        if (!pending_remote_forward_specs.empty()) {
            const auto failed = pending_remote_forward_specs.front();
            err << "remote forward rejected: "
                << failed.bind_addr << ':' << failed.bind_port
                << " -> " << failed.target_host << ':' << failed.target_port << '\n';
            return false;
        }
        return true;
    }

    bool SshCliForwardRuntime::handle_forwarded_tcpip_open(SocketHandle ssh_fd,
                                                           yuan::net::ssh::SshTransport &transport,
                                                           const yuan::net::ssh::SshChannelOpenMessage &open,
                                                           const std::function<void(const std::string &)> &debug,
                                                           bool &handled)
    {
        using namespace yuan::net::ssh;

        handled = false;
        if (open.channel_type != SSH_CHANNEL_FORWARDED_TCPIP) {
            return true;
        }
        handled = true;

        size_t offset = 0;
        auto connected_address = SshMessageCodec::read_string(
            open.type_specific_data.data(),
            open.type_specific_data.size(),
            offset);
        if (!connected_address || offset + 4 > open.type_specific_data.size()) {
            return true;
        }
        const uint32_t connected_port = SshMessageCodec::read_uint32(
            open.type_specific_data.data(),
            open.type_specific_data.size(),
            offset);
        auto originator_address = SshMessageCodec::read_string(
            open.type_specific_data.data(),
            open.type_specific_data.size(),
            offset);
        if (!originator_address || offset + 4 > open.type_specific_data.size()) {
            return true;
        }
        (void)SshMessageCodec::read_uint32(
            open.type_specific_data.data(),
            open.type_specific_data.size(),
            offset);

        std::optional<RemoteForwardSpec> matched_spec;
        for (const auto &spec : remote_forward_specs) {
            if (spec.bind_addr == *connected_address && spec.bind_port == static_cast<uint16_t>(connected_port)) {
                matched_spec = spec;
                break;
            }
        }
        if (!matched_spec) {
            debug("remote-forward open did not match " + *connected_address + ":" + std::to_string(connected_port));
            SshChannelOpenFailureMessage fail;
            fail.recipient_channel = open.sender_channel;
            fail.reason_code = static_cast<uint32_t>(SshChannelOpenFailureReason::SSH_OPEN_CONNECT_FAILED);
            fail.description = "No matching -R forwarding target";
            fail.language = "en";
            (void)send_packet(ssh_fd, transport, SshMessageCodec::encode_channel_open_failure(fail));
            return true;
        }

        SocketGuard forward_sock;
        if (!connect_tcp(matched_spec->target_host, matched_spec->target_port, forward_sock)) {
            debug("remote-forward target connect failed " + matched_spec->target_host + ":" +
                  std::to_string(matched_spec->target_port));
            SshChannelOpenFailureMessage fail;
            fail.recipient_channel = open.sender_channel;
            fail.reason_code = static_cast<uint32_t>(SshChannelOpenFailureReason::SSH_OPEN_CONNECT_FAILED);
            fail.description = "Remote forward target connect failed";
            fail.language = "en";
            (void)send_packet(ssh_fd, transport, SshMessageCodec::encode_channel_open_failure(fail));
            return true;
        }

        SshChannelOpenConfirmationMessage conf;
        conf.recipient_channel = open.sender_channel;
        const uint32_t local_forward_id = next_forward_local_channel_id++;
        conf.sender_channel = local_forward_id;
        conf.initial_window_size = open.initial_window_size;
        conf.maximum_packet_size = open.maximum_packet_size;
        (void)send_packet(ssh_fd, transport, SshMessageCodec::encode_channel_open_confirmation(conf));
        debug("remote-forward open confirmed local=" + std::to_string(local_forward_id) +
              " remote=" + std::to_string(open.sender_channel));

        forward_local_to_remote[local_forward_id] = open.sender_channel;
        forward_remote_to_local[open.sender_channel] = local_forward_id;
        forward_local_to_socket[local_forward_id] = forward_sock.fd;
        forward_sock.fd = kInvalidSocket;
        return true;
    }

    bool SshCliForwardRuntime::handle_channel_data(SocketHandle ssh_fd,
                                                   yuan::net::ssh::SshTransport &transport,
                                                   const yuan::net::ssh::SshChannelDataMessage &data_msg,
                                                   const std::function<void(const std::string &)> &debug,
                                                   bool &handled)
    {
        using namespace yuan::net::ssh;

        handled = false;
        auto forward_it = forward_local_to_socket.find(data_msg.recipient_channel);
        if (forward_it == forward_local_to_socket.end() || forward_it->second == kInvalidSocket) {
            return true;
        }
        handled = true;
        debug("local-forward recv data bytes=" + std::to_string(data_msg.data.size()) +
              " local=" + std::to_string(data_msg.recipient_channel));
        if (!send_all(forward_it->second, data_msg.data.data(), data_msg.data.size())) {
            auto remote_it = forward_local_to_remote.find(data_msg.recipient_channel);
            if (remote_it != forward_local_to_remote.end()) {
                SshChannelCloseMessage close_msg;
                close_msg.recipient_channel = remote_it->second;
                (void)send_packet(ssh_fd, transport, SshMessageCodec::encode_channel_close(close_msg));
            }
            close_channel(data_msg.recipient_channel);
        }
        return true;
    }

    bool SshCliForwardRuntime::handle_channel_eof(const yuan::net::ssh::SshChannelEofMessage &eof_msg)
    {
        auto forward_it = forward_local_to_socket.find(eof_msg.recipient_channel);
        if (forward_it != forward_local_to_socket.end()) {
            shutdown_socket_write(forward_it->second);
        }
        return true;
    }

    bool SshCliForwardRuntime::handle_channel_close(SocketHandle ssh_fd,
                                                    yuan::net::ssh::SshTransport &transport,
                                                    const yuan::net::ssh::SshChannelCloseMessage &close_msg,
                                                    uint32_t session_local_channel_id,
                                                    bool &handled)
    {
        using namespace yuan::net::ssh;

        handled = false;
        if (close_msg.recipient_channel == session_local_channel_id) {
            return true;
        }

        auto remote_it = forward_local_to_remote.find(close_msg.recipient_channel);
        SshChannelCloseMessage back;
        back.recipient_channel = remote_it != forward_local_to_remote.end()
                                 ? remote_it->second
                                 : close_msg.recipient_channel;
        (void)send_packet(ssh_fd, transport, SshMessageCodec::encode_channel_close(back));
        close_channel(close_msg.recipient_channel);
        handled = true;
        return true;
    }
}
