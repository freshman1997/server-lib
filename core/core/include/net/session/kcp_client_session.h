#ifndef YUAN_NET_KCP_CLIENT_SESSION_H
#define YUAN_NET_KCP_CLIENT_SESSION_H

#include "ikcp.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace yuan::net
{
    class KcpClientSession final
    {
    public:
        struct Config
        {
            std::string host = "127.0.0.1";
            std::uint16_t port = 0;
            std::vector<std::uint8_t> handshake_payload;
            std::uint8_t handshake_packet_type = 1;
            std::uint8_t handshake_ack_packet_type = 2;
            std::uint8_t kcp_packet_type = 3;
            std::uint32_t handshake_timeout_ms = 2000;
            std::uint32_t update_interval_ms = 10;
            std::size_t recv_buffer_size = 64 * 1024;
            std::uint32_t mtu = 1400;
            std::uint32_t send_window = 128;
            std::uint32_t receive_window = 128;
            bool nodelay = true;
            std::uint32_t resend = 1;
            bool no_congestion_control = true;
        };

        using DataCallback = std::function<void(std::vector<std::uint8_t> payload)>;

        KcpClientSession();
        ~KcpClientSession();

        KcpClientSession(const KcpClientSession &) = delete;
        KcpClientSession &operator=(const KcpClientSession &) = delete;

        bool connect(Config config);
        void close();
        void poll();
        bool send(const std::vector<std::uint8_t> &payload);

        [[nodiscard]] bool connected() const noexcept { return kcp_ != nullptr && socket_ != invalid_socket(); }
        [[nodiscard]] std::uint32_t conv() const noexcept { return conv_; }
        [[nodiscard]] const std::string &last_error() const noexcept { return last_error_; }

        void set_data_callback(DataCallback callback);

    private:
#ifdef _WIN32
        using SocketHandle = SOCKET;
#else
        using SocketHandle = int;
#endif

        struct OutputContext
        {
            SocketHandle socket = invalid_socket();
            sockaddr_storage server_addr{};
            socklen_t server_addr_len = 0;
            std::uint8_t packet_type = 3;
        };

        static constexpr SocketHandle invalid_socket()
        {
#ifdef _WIN32
            return INVALID_SOCKET;
#else
            return -1;
#endif
        }

        static int kcp_output(const char *buf, int len, ikcpcb *kcp, void *user);
        static bool ensure_sockets_ready();
        static void close_socket(SocketHandle socket);
        static bool wait_readable(SocketHandle socket);
        static std::uint32_t read_u32_be(const std::uint8_t *data);
        static IUINT32 now_ms();

        bool open_socket();
        bool send_handshake();
        bool read_handshake_ack();
        void read_available_packets();
        void receive_payloads();
        void release_kcp();

        Config config_;
        DataCallback data_callback_;
        SocketHandle socket_ = invalid_socket();
        OutputContext output_context_;
        ikcpcb *kcp_ = nullptr;
        std::uint32_t conv_ = 0;
        std::string last_error_;
    };
}

#endif
