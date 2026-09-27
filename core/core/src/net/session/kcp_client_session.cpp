#include "net/session/kcp_client_session.h"

#include <chrono>
#include <cstring>

namespace yuan::net
{
    KcpClientSession::KcpClientSession() = default;

    KcpClientSession::~KcpClientSession()
    {
        close();
    }

    bool KcpClientSession::connect(Config config)
    {
        close();
        config_ = std::move(config);
        last_error_.clear();
        if (config_.port == 0) {
            last_error_ = "kcp server port is zero";
            return false;
        }
        if (config_.recv_buffer_size == 0) {
            config_.recv_buffer_size = 64 * 1024;
        }
        if (config_.handshake_timeout_ms == 0) {
            config_.handshake_timeout_ms = 2000;
        }
        if (!open_socket() || !send_handshake() || !read_handshake_ack()) {
            close();
            return false;
        }

        kcp_ = ikcp_create(conv_, &output_context_);
        if (kcp_ == nullptr) {
            last_error_ = "failed to create kcp session";
            close();
            return false;
        }
        kcp_->output = &KcpClientSession::kcp_output;
        if (config_.mtu > 0) {
            (void)ikcp_setmtu(kcp_, static_cast<int>(config_.mtu));
        }
        ikcp_wndsize(kcp_, static_cast<int>(config_.send_window), static_cast<int>(config_.receive_window));
        ikcp_nodelay(kcp_,
                     config_.nodelay ? 1 : 0,
                     static_cast<int>(config_.update_interval_ms == 0 ? 10 : config_.update_interval_ms),
                     static_cast<int>(config_.resend),
                     config_.no_congestion_control ? 1 : 0);
        return true;
    }

    void KcpClientSession::close()
    {
        release_kcp();
        if (socket_ != invalid_socket()) {
            close_socket(socket_);
            socket_ = invalid_socket();
        }
        output_context_ = {};
        conv_ = 0;
    }

    void KcpClientSession::poll()
    {
        if (!connected()) {
            return;
        }
        ikcp_update(kcp_, now_ms());
        read_available_packets();
        receive_payloads();
    }

    bool KcpClientSession::send(const std::vector<std::uint8_t> &payload)
    {
        if (!connected()) {
            last_error_ = "kcp session is not connected";
            return false;
        }
        if (ikcp_send(kcp_, reinterpret_cast<const char *>(payload.data()), static_cast<int>(payload.size())) < 0) {
            last_error_ = "kcp send failed";
            return false;
        }
        ikcp_update(kcp_, now_ms());
        ikcp_flush(kcp_);
        return true;
    }

    void KcpClientSession::set_data_callback(DataCallback callback)
    {
        data_callback_ = std::move(callback);
    }

    bool KcpClientSession::open_socket()
    {
        if (!ensure_sockets_ready()) {
            last_error_ = "failed to initialize sockets";
            return false;
        }

        addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_DGRAM;
        addrinfo *result = nullptr;
        const auto port = std::to_string(config_.port);
        if (getaddrinfo(config_.host.c_str(), port.c_str(), &hints, &result) != 0 || result == nullptr) {
            last_error_ = "failed to resolve kcp endpoint " + config_.host + ":" + port;
            return false;
        }

        socket_ = socket(result->ai_family, result->ai_socktype, result->ai_protocol);
        if (socket_ == invalid_socket()) {
            freeaddrinfo(result);
            last_error_ = "failed to create udp socket";
            return false;
        }

        output_context_.socket = socket_;
        output_context_.server_addr_len = static_cast<socklen_t>(result->ai_addrlen);
        output_context_.packet_type = config_.kcp_packet_type;
        std::memcpy(&output_context_.server_addr, result->ai_addr, result->ai_addrlen);
        freeaddrinfo(result);
        return true;
    }

    bool KcpClientSession::send_handshake()
    {
        std::vector<std::uint8_t> packet;
        packet.reserve(config_.handshake_payload.size() + 1);
        packet.push_back(config_.handshake_packet_type);
        packet.insert(packet.end(), config_.handshake_payload.begin(), config_.handshake_payload.end());
        const auto sent = sendto(socket_,
                                 reinterpret_cast<const char *>(packet.data()),
                                 static_cast<int>(packet.size()),
                                 0,
                                 reinterpret_cast<const sockaddr *>(&output_context_.server_addr),
                                 output_context_.server_addr_len);
        if (sent != static_cast<decltype(sent)>(packet.size())) {
            last_error_ = "failed to send kcp handshake";
            return false;
        }
        return true;
    }

    bool KcpClientSession::read_handshake_ack()
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(config_.handshake_timeout_ms);
        while (std::chrono::steady_clock::now() < deadline) {
            if (!wait_readable(socket_)) {
                continue;
            }
            std::uint8_t buffer[64]{};
            const auto received = recv(socket_, reinterpret_cast<char *>(buffer), sizeof(buffer), 0);
            if (received == 5 && buffer[0] == config_.handshake_ack_packet_type) {
                conv_ = read_u32_be(buffer + 1);
                if (conv_ == 0) {
                    last_error_ = "kcp handshake returned invalid conv";
                    return false;
                }
                return true;
            }
        }
        last_error_ = "timed out waiting for kcp handshake ack";
        return false;
    }

    void KcpClientSession::read_available_packets()
    {
        while (connected() && wait_readable(socket_)) {
            std::vector<std::uint8_t> packet(config_.recv_buffer_size);
            const auto received = recv(socket_, reinterpret_cast<char *>(packet.data()), static_cast<int>(packet.size()), 0);
            if (received <= 0) {
                last_error_ = "kcp udp receive failed";
                close();
                return;
            }
            packet.resize(static_cast<std::size_t>(received));
            if (packet.empty() || packet.front() != config_.kcp_packet_type) {
                continue;
            }
            if (ikcp_input(kcp_, reinterpret_cast<const char *>(packet.data() + 1), static_cast<long>(packet.size() - 1)) < 0) {
                last_error_ = "kcp input failed";
                close();
                return;
            }
        }
    }

    void KcpClientSession::receive_payloads()
    {
        if (!connected()) {
            return;
        }
        std::vector<char> buffer(config_.recv_buffer_size);
        for (;;) {
            const auto received = ikcp_recv(kcp_, buffer.data(), static_cast<int>(buffer.size()));
            if (received <= 0) {
                return;
            }
            if (data_callback_) {
                data_callback_(std::vector<std::uint8_t>(reinterpret_cast<std::uint8_t *>(buffer.data()),
                                                         reinterpret_cast<std::uint8_t *>(buffer.data()) + received));
            }
        }
    }

    void KcpClientSession::release_kcp()
    {
        if (kcp_ != nullptr) {
            ikcp_release(kcp_);
            kcp_ = nullptr;
        }
    }

    int KcpClientSession::kcp_output(const char *buf, int len, ikcpcb *, void *user)
    {
        auto *context = static_cast<OutputContext *>(user);
        if (context == nullptr || context->socket == invalid_socket() || len <= 0) {
            return -1;
        }
        std::vector<std::uint8_t> packet;
        packet.reserve(static_cast<std::size_t>(len) + 1);
        packet.push_back(context->packet_type);
        packet.insert(packet.end(), reinterpret_cast<const std::uint8_t *>(buf), reinterpret_cast<const std::uint8_t *>(buf) + len);
        const auto sent = sendto(context->socket,
                                 reinterpret_cast<const char *>(packet.data()),
                                 static_cast<int>(packet.size()),
                                 0,
                                 reinterpret_cast<const sockaddr *>(&context->server_addr),
                                 context->server_addr_len);
        return sent == static_cast<decltype(sent)>(packet.size()) ? len : -1;
    }

    bool KcpClientSession::ensure_sockets_ready()
    {
#ifdef _WIN32
        static const bool ready = [] {
            WSADATA data{};
            return WSAStartup(MAKEWORD(2, 2), &data) == 0;
        }();
        return ready;
#else
        return true;
#endif
    }

    void KcpClientSession::close_socket(SocketHandle socket)
    {
#ifdef _WIN32
        closesocket(socket);
#else
        ::close(socket);
#endif
    }

    bool KcpClientSession::wait_readable(SocketHandle socket)
    {
        fd_set read_set;
        FD_ZERO(&read_set);
        FD_SET(socket, &read_set);
        timeval timeout{0, 0};
#ifdef _WIN32
        return select(0, &read_set, nullptr, nullptr, &timeout) > 0;
#else
        return select(socket + 1, &read_set, nullptr, nullptr, &timeout) > 0;
#endif
    }

    std::uint32_t KcpClientSession::read_u32_be(const std::uint8_t *data)
    {
        return (static_cast<std::uint32_t>(data[0]) << 24U) |
               (static_cast<std::uint32_t>(data[1]) << 16U) |
               (static_cast<std::uint32_t>(data[2]) << 8U) |
               static_cast<std::uint32_t>(data[3]);
    }

    IUINT32 KcpClientSession::now_ms()
    {
        return static_cast<IUINT32>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
    }
}
