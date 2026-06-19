#ifndef YUAN_NET_SESSION_KCP_SERVER_SESSION_H
#define YUAN_NET_SESSION_KCP_SERVER_SESSION_H

#include "buffer/byte_buffer.h"
#include "ikcp.h"
#include "net/runtime/network_runtime.h"
#include "net/session/datagram_server_session.h"
#include "net/socket/inet_address.h"
#include "timer/timer_handle.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace yuan::net
{
    class KcpServerSession final
    {
    public:
        struct Config
        {
            std::string host = "127.0.0.1";
            std::uint16_t port = 0;
            std::vector<std::uint8_t> handshake_magic{'Y', 'K', 'C', 'P', '1'};
            std::uint8_t handshake_packet_type = 1;
            std::uint8_t handshake_ack_packet_type = 2;
            std::uint8_t kcp_packet_type = 3;
            std::uint32_t update_interval_ms = 10;
            std::uint32_t cleanup_interval_ms = 1000;
            std::uint64_t idle_timeout_ms = 60000;
            std::uint64_t first_connection_id = 1;
            std::size_t recv_buffer_size = 64 * 1024;
            std::uint32_t mtu = 1400;
            std::uint32_t send_window = 128;
            std::uint32_t receive_window = 128;
            bool nodelay = true;
            std::uint32_t resend = 1;
            bool no_congestion_control = true;
            std::size_t max_sessions = 0;
            std::size_t max_sessions_per_address = 1;
            std::size_t max_sessions_per_ip = 0;
            bool allow_migration = false;
            std::size_t max_handshakes_per_address_per_window = 0;
            std::uint64_t handshake_rate_window_ms = 1000;
            std::size_t max_malformed_packets_per_address = 0;
        };

        struct Metrics
        {
            std::uint64_t handshakes_accepted = 0;
            std::uint64_t handshakes_rejected = 0;
            std::uint64_t handshakes_rate_limited = 0;
            std::uint64_t sessions_created = 0;
            std::uint64_t sessions_closed = 0;
            std::uint64_t malformed_packets = 0;
            std::uint64_t kcp_input_errors = 0;
            std::uint64_t payloads_received = 0;
            std::uint64_t payloads_sent = 0;
            std::uint64_t active_sessions = 0;
        };

        using DataCallback = std::function<void(std::uint64_t connection_id, std::vector<std::uint8_t> payload)>;
        using CloseCallback = std::function<void(std::uint64_t connection_id)>;
        struct HandshakeDecision
        {
            bool accepted = false;
            std::uint32_t migrate_conv = 0;
        };
        using HandshakeValidator = std::function<HandshakeDecision(const InetAddress &address, const std::vector<std::uint8_t> &payload)>;

        explicit KcpServerSession(NetworkRuntime &runtime);
        ~KcpServerSession();

        KcpServerSession(const KcpServerSession &) = delete;
        KcpServerSession &operator=(const KcpServerSession &) = delete;

        bool start(Config config);
        void stop();

        bool send(std::uint64_t connection_id, const std::vector<std::uint8_t> &payload);
        bool close(std::uint64_t connection_id);
        void close_all();

        [[nodiscard]] std::size_t active_connection_count() const;
        [[nodiscard]] bool started() const noexcept { return started_; }
        [[nodiscard]] Metrics metrics() const;

        void set_data_callback(DataCallback callback);
        void set_close_callback(CloseCallback callback);
        void set_handshake_validator(HandshakeValidator validator);

    private:
        struct Session
        {
            std::uint32_t conv = 0;
            std::uint64_t connection_id = 0;
            InetAddress address;
            ikcpcb *kcp = nullptr;
            std::uint64_t last_activity_ms = 0;
        };

        void on_datagram(ConnectionContext &context);
        void handle_handshake(const InetAddress &address, const std::vector<std::uint8_t> &payload);
        bool migrate_session(std::uint32_t conv, const InetAddress &address);
        void handle_kcp_packet(const InetAddress &address, std::vector<std::uint8_t> packet);
        void update_sessions();
        void cleanup_idle_sessions();
        bool erase_session(std::uint64_t connection_id, bool notify);
        void send_packet(const InetAddress &address, std::uint8_t type, const void *data, std::size_t size);
        bool handshake_rate_allowed(const InetAddress &address);
        bool too_many_sessions_for_address(const InetAddress &address) const;
        bool too_many_sessions_for_ip(const InetAddress &address) const;
        void account_malformed(const InetAddress &address);
        [[nodiscard]] std::uint32_t next_conv();
        [[nodiscard]] std::string address_key(const InetAddress &address) const;
        [[nodiscard]] Session *session_for_connection(std::uint64_t connection_id);
        [[nodiscard]] Session *session_for_conv(std::uint32_t conv);
        static int kcp_output(const char *buf, int len, ikcpcb *kcp, void *user);

        NetworkRuntime &runtime_;
        DatagramServerSession udp_session_;
        Config config_;
        DataCallback data_callback_;
        CloseCallback close_callback_;
        HandshakeValidator handshake_validator_;
        Metrics metrics_;
        mutable std::mutex mutex_;
        std::unordered_map<std::uint32_t, std::unique_ptr<Session>> sessions_by_conv_;
        std::unordered_map<std::uint64_t, std::uint32_t> conv_by_connection_;
        std::unordered_map<std::string, std::uint32_t> conv_by_address_;
        struct AddressRateState
        {
            std::uint64_t window_start_ms = 0;
            std::size_t handshakes = 0;
            std::size_t malformed = 0;
        };
        std::unordered_map<std::string, AddressRateState> address_rates_;
        timer::TimerHandle update_timer_;
        timer::TimerHandle cleanup_timer_;
        std::atomic<std::uint64_t> next_connection_id_{1};
        std::atomic<std::uint32_t> next_conv_{1};
        bool started_ = false;
    };
}

#endif
