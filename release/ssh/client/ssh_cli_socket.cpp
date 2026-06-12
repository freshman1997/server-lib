#include "ssh_cli_socket.h"

#include "platform/native_platform.h"
#include "ssh_cli_terminal.h"

#include <array>
#include <chrono>
#include <cstdlib>
#include <thread>

#ifdef _WIN32
#include <ws2tcpip.h>
#else
#include <fcntl.h>
#include <netdb.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace yuan::release_ssh::client
{
    SocketGuard::~SocketGuard()
    {
        close();
    }

    void SocketGuard::close()
    {
        if (fd == kInvalidSocket) {
            return;
        }
#ifdef _WIN32
        closesocket(fd);
#else
        ::close(fd);
#endif
        fd = kInvalidSocket;
    }

    bool set_recv_timeout(SocketHandle fd, int timeout_ms)
    {
#ifdef _WIN32
        const DWORD timeout = static_cast<DWORD>(timeout_ms);
        return setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char *>(&timeout), sizeof(timeout)) == 0;
#else
        timeval tv{};
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        return setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) == 0;
#endif
    }

    bool connect_tcp(const std::string &host, uint16_t port, SocketGuard &sock)
    {
        addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;

        addrinfo *result = nullptr;
        const std::string service = std::to_string(port);
        if (getaddrinfo(host.c_str(), service.c_str(), &hints, &result) != 0) {
            return false;
        }

        bool ok = false;
        for (addrinfo *it = result; it != nullptr; it = it->ai_next) {
            SocketHandle fd = socket(it->ai_family, it->ai_socktype, it->ai_protocol);
            if (fd == kInvalidSocket) {
                continue;
            }

#ifdef _WIN32
            const int rc = ::connect(fd, it->ai_addr, static_cast<int>(it->ai_addrlen));
#else
            const int rc = ::connect(fd, it->ai_addr, it->ai_addrlen);
#endif
            if (rc == 0) {
                sock.close();
                sock.fd = fd;
                ok = true;
                break;
            }

#ifdef _WIN32
            closesocket(fd);
#else
            ::close(fd);
#endif
        }

        freeaddrinfo(result);
        return ok;
    }

    bool socket_read_ready(SocketHandle fd)
    {
        if (fd == kInvalidSocket) {
            return false;
        }

        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(fd, &readfds);
        timeval tv{};
#ifdef _WIN32
        const int rc = select(0, &readfds, nullptr, nullptr, &tv);
#else
        const int rc = select(fd + 1, &readfds, nullptr, nullptr, &tv);
#endif
        return rc > 0 && FD_ISSET(fd, &readfds);
    }

    bool socket_would_block_last_error()
    {
        const int err = yuan::platform::GetLastNativeError();
        if (yuan::platform::ClassifyNativeError(err) == yuan::platform::NativeError::timed_out) {
            return true;
        }
        return yuan::platform::IsNativeRetryableError(err);
    }

    void shutdown_socket_write(SocketHandle fd)
    {
        if (fd == kInvalidSocket) {
            return;
        }
#ifdef _WIN32
        (void)shutdown(fd, SD_SEND);
#else
        (void)shutdown(fd, SHUT_WR);
#endif
    }

    void close_socket_handle(SocketHandle &fd)
    {
        if (fd == kInvalidSocket) {
            return;
        }
#ifdef _WIN32
        closesocket(fd);
#else
        ::close(fd);
#endif
        fd = kInvalidSocket;
    }

    bool set_socket_nonblocking(SocketHandle fd)
    {
        if (fd == kInvalidSocket) {
            return false;
        }
#ifdef _WIN32
        u_long mode = 1;
        return ioctlsocket(fd, FIONBIO, &mode) == 0;
#else
        const int flags = fcntl(fd, F_GETFL, 0);
        if (flags < 0) {
            return false;
        }
        return fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
#endif
    }

    bool create_listen_socket(const std::string &bind_addr,
                              uint16_t bind_port,
                              SocketHandle &out_fd)
    {
        out_fd = kInvalidSocket;

        addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_flags = AI_PASSIVE;

        addrinfo *result = nullptr;
        const std::string service = std::to_string(bind_port);
        const char *host = bind_addr.empty() ? nullptr : bind_addr.c_str();
        if (getaddrinfo(host, service.c_str(), &hints, &result) != 0) {
            return false;
        }

        bool ok = false;
        for (addrinfo *it = result; it != nullptr; it = it->ai_next) {
            SocketHandle fd = socket(it->ai_family, it->ai_socktype, it->ai_protocol);
            if (fd == kInvalidSocket) {
                continue;
            }

            int reuse = 1;
            (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR,
                             reinterpret_cast<const char *>(&reuse),
                             sizeof(reuse));

#ifdef _WIN32
            if (::bind(fd, it->ai_addr, static_cast<int>(it->ai_addrlen)) != 0) {
                closesocket(fd);
                continue;
            }
#else
            if (::bind(fd, it->ai_addr, it->ai_addrlen) != 0) {
                ::close(fd);
                continue;
            }
#endif

            if (listen(fd, 128) != 0 || !set_socket_nonblocking(fd)) {
#ifdef _WIN32
                closesocket(fd);
#else
                ::close(fd);
#endif
                continue;
            }

            out_fd = fd;
            ok = true;
            break;
        }

        freeaddrinfo(result);
        return ok;
    }

    bool accept_pending_client(SocketHandle listen_fd,
                               SocketHandle &accepted_fd,
                               std::string &origin_host,
                               uint16_t &origin_port,
                               bool &had_fatal_error)
    {
        accepted_fd = kInvalidSocket;
        origin_host = "127.0.0.1";
        origin_port = 0;
        had_fatal_error = false;

        sockaddr_storage addr{};
#ifdef _WIN32
        int addr_len = static_cast<int>(sizeof(addr));
#else
        socklen_t addr_len = sizeof(addr);
#endif
        SocketHandle fd = accept(listen_fd, reinterpret_cast<sockaddr *>(&addr), &addr_len);
        if (fd == kInvalidSocket) {
            const int err = yuan::platform::GetLastNativeError();
            const auto kind = yuan::platform::ClassifyNativeError(err);
            if (socket_would_block_last_error() || kind == yuan::platform::NativeError::connection_aborted) {
                return false;
            }
            had_fatal_error = true;
            return false;
        }

        if (!set_socket_nonblocking(fd)) {
            close_socket_handle(fd);
            had_fatal_error = true;
            return false;
        }

        char host_buf[NI_MAXHOST] = {};
        char serv_buf[NI_MAXSERV] = {};
        if (getnameinfo(reinterpret_cast<const sockaddr *>(&addr),
                        addr_len,
                        host_buf,
                        sizeof(host_buf),
                        serv_buf,
                        sizeof(serv_buf),
                        NI_NUMERICHOST | NI_NUMERICSERV) == 0) {
            origin_host = host_buf;
            char *end = nullptr;
            unsigned long parsed = std::strtoul(serv_buf, &end, 10);
            if (end != serv_buf && *end == '\0' && parsed <= 65535ul) {
                origin_port = static_cast<uint16_t>(parsed);
            }
        }

        accepted_fd = fd;
        return true;
    }

    bool send_all(SocketHandle fd, const uint8_t *data, size_t len)
    {
        size_t sent = 0;
        while (sent < len) {
#ifdef _WIN32
            const int n = send(fd, reinterpret_cast<const char *>(data + sent), static_cast<int>(len - sent), 0);
#else
            const ssize_t n = send(fd, data + sent, len - sent, 0);
#endif
            if (n <= 0) {
                return false;
            }
            sent += static_cast<size_t>(n);
        }
        return true;
    }

    RecvStatus recv_some(SocketHandle fd, std::vector<uint8_t> &out)
    {
        std::array<uint8_t, 64 * 1024> buf{};
#ifdef _WIN32
        const int n = recv(fd, reinterpret_cast<char *>(buf.data()), static_cast<int>(buf.size()), 0);
#else
        const ssize_t n = recv(fd, buf.data(), buf.size(), 0);
#endif
        if (n <= 0) {
#ifdef _WIN32
            const int err = yuan::platform::GetLastNativeError();
            if (yuan::platform::IsNativeRetryableError(err) ||
                yuan::platform::ClassifyNativeError(err) == yuan::platform::NativeError::timed_out) {
                return RecvStatus::timeout;
            }
#else
            const int err = yuan::platform::GetLastNativeError();
            if (yuan::platform::IsNativeRetryableError(err)) {
                return RecvStatus::timeout;
            }
#endif
            return RecvStatus::closed_or_error;
        }
        out.assign(buf.begin(), buf.begin() + n);
        return RecvStatus::data;
    }

    bool recv_line(SocketHandle fd, std::string &line)
    {
        line.clear();
        std::array<char, 1> byte{};
        while (line.size() < 1024) {
#ifdef _WIN32
            const int n = recv(fd, byte.data(), 1, 0);
#else
            const ssize_t n = recv(fd, byte.data(), 1, 0);
#endif
            if (n <= 0) {
                return false;
            }
            line.push_back(byte[0]);
            if (line.size() >= 2 && line[line.size() - 2] == '\r' && line.back() == '\n') {
                return true;
            }
        }
        return false;
    }

    PacketReadStatus read_packet(SocketHandle fd,
                                 yuan::buffer::ByteBuffer &recv_buf,
                                 yuan::net::ssh::SshTransport &transport,
                                 std::vector<uint8_t> &payload)
    {
        for (;;) {
            auto parse = transport.try_parse_packet(recv_buf);
            if (parse.invalid) {
                return PacketReadStatus::eof_or_error;
            }
            if (!parse.complete) {
                std::vector<uint8_t> chunk;
                const auto recv_status = recv_some(fd, chunk);
                if (recv_status == RecvStatus::timeout) {
                    return PacketReadStatus::timeout;
                }
                if (recv_status != RecvStatus::data) {
                    return PacketReadStatus::eof_or_error;
                }
                recv_buf.append(chunk.data(), chunk.size());
                continue;
            }

            auto decoded = transport.decode_packet(
                reinterpret_cast<const uint8_t *>(recv_buf.read_ptr()),
                parse.total_bytes);
            transport.increment_recv_seq();
            recv_buf.consume(parse.total_bytes);

            if (!decoded) {
                return PacketReadStatus::eof_or_error;
            }
            payload = std::move(*decoded);
            return PacketReadStatus::ok;
        }
    }

    ClientIoReady wait_client_io(SocketHandle fd,
                                 bool watch_stdin,
                                 int timeout_ms)
    {
        ClientIoReady ready;
#ifdef _WIN32
        ready.stdin_ready = watch_stdin && stdin_has_data_nonblocking();
        if (socket_read_ready(fd)) {
            ready.socket_ready = true;
            return ready;
        }
        if (ready.stdin_ready || timeout_ms <= 0) {
            return ready;
        }
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        while (std::chrono::steady_clock::now() < deadline) {
            if (socket_read_ready(fd)) {
                ready.socket_ready = true;
                return ready;
            }
            if (watch_stdin && stdin_has_data_nonblocking()) {
                ready.stdin_ready = true;
                return ready;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return ready;
#else
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(fd, &readfds);
        int max_fd = fd;
        if (watch_stdin) {
            FD_SET(STDIN_FILENO, &readfds);
            if (STDIN_FILENO > max_fd) {
                max_fd = STDIN_FILENO;
            }
        }

        timeval tv{};
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        const int rc = select(max_fd + 1, &readfds, nullptr, nullptr, &tv);
        if (rc > 0) {
            ready.socket_ready = FD_ISSET(fd, &readfds);
            ready.stdin_ready = watch_stdin && FD_ISSET(STDIN_FILENO, &readfds);
        }
        return ready;
#endif
    }

    bool send_packet(SocketHandle fd,
                     yuan::net::ssh::SshTransport &transport,
                     const yuan::buffer::ByteBuffer &payload)
    {
        auto packet = transport.encode_packet(
            reinterpret_cast<const uint8_t *>(payload.read_ptr()),
            payload.readable_bytes());
        transport.increment_send_seq();
        return send_all(fd,
                        reinterpret_cast<const uint8_t *>(packet.read_ptr()),
                        packet.readable_bytes());
    }
}
