#ifndef YUAN_RELEASE_SSH_CLI_SOCKET_H
#define YUAN_RELEASE_SSH_CLI_SOCKET_H

#include "buffer/byte_buffer.h"
#include "transport/ssh_transport.h"

#include <cstdint>
#include <string>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#endif

namespace yuan::release_ssh::client
{
#ifdef _WIN32
    using SocketHandle = SOCKET;
    constexpr SocketHandle kInvalidSocket = INVALID_SOCKET;
#else
    using SocketHandle = int;
    constexpr SocketHandle kInvalidSocket = -1;
#endif

    struct SocketGuard
    {
        SocketHandle fd = kInvalidSocket;

        ~SocketGuard();
        void close();
    };

    enum class RecvStatus {
        data,
        timeout,
        closed_or_error
    };

    enum class PacketReadStatus {
        ok,
        timeout,
        eof_or_error
    };

    struct ClientIoReady
    {
        bool socket_ready = false;
        bool stdin_ready = false;
    };

    bool set_recv_timeout(SocketHandle fd, int timeout_ms);
    bool connect_tcp(const std::string &host, uint16_t port, SocketGuard &sock);
    bool socket_read_ready(SocketHandle fd);
    bool socket_would_block_last_error();
    void shutdown_socket_write(SocketHandle fd);
    void close_socket_handle(SocketHandle &fd);
    bool set_socket_nonblocking(SocketHandle fd);
    bool create_listen_socket(const std::string &bind_addr,
                              uint16_t bind_port,
                              SocketHandle &out_fd);
    bool accept_pending_client(SocketHandle listen_fd,
                               SocketHandle &accepted_fd,
                               std::string &origin_host,
                               uint16_t &origin_port,
                               bool &had_fatal_error);
    bool send_all(SocketHandle fd, const uint8_t *data, size_t len);
    RecvStatus recv_some(SocketHandle fd, std::vector<uint8_t> &out);
    bool recv_line(SocketHandle fd, std::string &line);
    PacketReadStatus read_packet(SocketHandle fd,
                                 yuan::buffer::ByteBuffer &recv_buf,
                                 yuan::net::ssh::SshTransport &transport,
                                 std::vector<uint8_t> &payload);
    ClientIoReady wait_client_io(SocketHandle fd,
                                 bool watch_stdin,
                                 int timeout_ms);
    bool send_packet(SocketHandle fd,
                     yuan::net::ssh::SshTransport &transport,
                     const yuan::buffer::ByteBuffer &payload);
}

#endif
