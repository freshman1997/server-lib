#ifndef YUAN_GAME_SERVER_COMMON_TCP_RPC_H
#define YUAN_GAME_SERVER_COMMON_TCP_RPC_H

#include "yuan/rpc/rpc.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace yuan::game::server::tcp_rpc
{
    inline void close_fd(int fd)
    {
        if (fd >= 0) {
            ::close(fd);
        }
    }

    inline bool read_exact(int fd, yuan::rpc::Bytes &out, std::size_t size)
    {
        out.resize(size);
        std::size_t offset = 0;
        while (offset < size) {
            const auto n = ::recv(fd, out.data() + offset, size - offset, 0);
            if (n <= 0) {
                return false;
            }
            offset += static_cast<std::size_t>(n);
        }
        return true;
    }

    inline bool write_exact(int fd, const yuan::rpc::Bytes &bytes)
    {
        std::size_t offset = 0;
        while (offset < bytes.size()) {
            const auto n = ::send(fd, bytes.data() + offset, bytes.size() - offset, MSG_NOSIGNAL);
            if (n <= 0) {
                return false;
            }
            offset += static_cast<std::size_t>(n);
        }
        return true;
    }

    inline std::uint32_t frame_body_size(const yuan::rpc::Bytes &header)
    {
        constexpr std::size_t body_size_offset = 12;
        return (static_cast<std::uint32_t>(header[body_size_offset]) << 24) |
               (static_cast<std::uint32_t>(header[body_size_offset + 1]) << 16) |
               (static_cast<std::uint32_t>(header[body_size_offset + 2]) << 8) |
               static_cast<std::uint32_t>(header[body_size_offset + 3]);
    }

    inline int connect_loopback(std::uint16_t port)
    {
        const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) {
            return -1;
        }

        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(port);
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (::connect(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0) {
            close_fd(fd);
            return -1;
        }
        return fd;
    }

    inline int connect_loopback_retry(std::uint16_t port, std::chrono::milliseconds timeout)
    {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        do {
            const int fd = connect_loopback(port);
            if (fd >= 0) {
                return fd;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        } while (std::chrono::steady_clock::now() < deadline);
        return -1;
    }

    inline std::optional<yuan::rpc::Bytes> read_frame(int fd)
    {
        yuan::rpc::Bytes frame;
        if (!read_exact(fd, frame, yuan::rpc::wire::header_size)) {
            return std::nullopt;
        }
        const auto body_size = frame_body_size(frame);
        yuan::rpc::Bytes body;
        if (!read_exact(fd, body, body_size)) {
            return std::nullopt;
        }
        frame.insert(frame.end(), body.begin(), body.end());
        return frame;
    }

    inline std::optional<yuan::rpc::Response> call(std::uint16_t port, const yuan::rpc::Message &message)
    {
        const int fd = connect_loopback_retry(port, std::chrono::milliseconds(1000));
        if (fd < 0) {
            return std::nullopt;
        }

        yuan::rpc::Bytes request_frame;
        if (!yuan::rpc::wire::encode_message(message, request_frame) || !write_exact(fd, request_frame)) {
            close_fd(fd);
            return std::nullopt;
        }

        auto response_frame = read_frame(fd);
        close_fd(fd);
        if (!response_frame) {
            return std::nullopt;
        }

        auto decoded = yuan::rpc::wire::decode_frame(*response_frame);
        if (!decoded.ok) {
            return std::nullopt;
        }
        return yuan::rpc::wire::to_response(std::move(decoded.frame));
    }

    inline int listen_loopback(std::uint16_t port)
    {
        const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) {
            return -1;
        }
        int yes = 1;
        (void)::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(port);
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (::bind(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0 || ::listen(fd, 16) != 0) {
            close_fd(fd);
            return -1;
        }
        return fd;
    }

    inline bool serve_one(int listen_fd, yuan::rpc::Server &server)
    {
        const int client_fd = ::accept(listen_fd, nullptr, nullptr);
        if (client_fd < 0) {
            return false;
        }

        auto request_frame = read_frame(client_fd);
        if (!request_frame) {
            close_fd(client_fd);
            return false;
        }
        auto decoded = yuan::rpc::wire::decode_frame(*request_frame);
        if (!decoded.ok) {
            close_fd(client_fd);
            return false;
        }

        auto response = server.handle(yuan::rpc::wire::to_message(std::move(decoded.frame)));
        yuan::rpc::Bytes response_frame;
        const bool ok = yuan::rpc::wire::encode_response(response, response_frame) && write_exact(client_fd, response_frame);
        close_fd(client_fd);
        return ok;
    }

    inline bool serve_n(int listen_fd, yuan::rpc::Server &server, std::size_t count)
    {
        for (std::size_t i = 0; i < count; ++i) {
            if (!serve_one(listen_fd, server)) {
                return false;
            }
        }
        return true;
    }

    inline bool serve_n_concurrent(int listen_fd, yuan::rpc::Server &server, std::size_t count)
    {
        std::vector<std::thread> workers;
        workers.reserve(count);
        bool accepted = true;
        for (std::size_t i = 0; i < count; ++i) {
            const int client_fd = ::accept(listen_fd, nullptr, nullptr);
            if (client_fd < 0) {
                accepted = false;
                break;
            }
            workers.emplace_back([client_fd, &server] {
                auto request_frame = read_frame(client_fd);
                if (request_frame) {
                    auto decoded = yuan::rpc::wire::decode_frame(*request_frame);
                    if (decoded.ok) {
                        auto response = server.handle(yuan::rpc::wire::to_message(std::move(decoded.frame)));
                        yuan::rpc::Bytes response_frame;
                        (void)(yuan::rpc::wire::encode_response(response, response_frame) && write_exact(client_fd, response_frame));
                    }
                }
                close_fd(client_fd);
            });
        }
        for (auto &worker : workers) {
            if (worker.joinable()) {
                worker.join();
            }
        }
        return accepted && workers.size() == count;
    }
}

#endif
