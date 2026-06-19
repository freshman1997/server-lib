#include "net/session/kcp_server_session.h"

#include "base/time.h"

#include <algorithm>
#include <vector>

namespace yuan::net
{
    namespace
    {
        thread_local InetAddress *t_kcp_output_address = nullptr;

        std::uint32_t read_u32_be(const std::uint8_t *data)
        {
            return (static_cast<std::uint32_t>(data[0]) << 24) |
                   (static_cast<std::uint32_t>(data[1]) << 16) |
                   (static_cast<std::uint32_t>(data[2]) << 8) |
                   static_cast<std::uint32_t>(data[3]);
        }

        void write_u32_be(std::uint8_t *data, std::uint32_t value)
        {
            data[0] = static_cast<std::uint8_t>((value >> 24) & 0xffU);
            data[1] = static_cast<std::uint8_t>((value >> 16) & 0xffU);
            data[2] = static_cast<std::uint8_t>((value >> 8) & 0xffU);
            data[3] = static_cast<std::uint8_t>(value & 0xffU);
        }

        ::yuan::buffer::ByteBuffer to_buffer(std::uint8_t type, const void *data, std::size_t size)
        {
            ::yuan::buffer::ByteBuffer buffer(size + 1);
            buffer.append(reinterpret_cast<const char *>(&type), 1);
            if (data && size > 0) {
                buffer.append(data, size);
            }
            return buffer;
        }

        std::vector<std::uint8_t> to_bytes(const ::yuan::buffer::ByteBuffer &buffer)
        {
            const auto span = buffer.readable_span();
            const auto *data = reinterpret_cast<const std::uint8_t *>(span.data());
            return std::vector<std::uint8_t>(data, data + span.size());
        }
    }

    KcpServerSession::KcpServerSession(NetworkRuntime &runtime)
        : runtime_(runtime)
    {
    }

    KcpServerSession::~KcpServerSession()
    {
        stop();
    }

    bool KcpServerSession::start(Config config)
    {
        if (started_) {
            return true;
        }
        config_ = std::move(config);
        if (config_.update_interval_ms == 0) {
            config_.update_interval_ms = 10;
        }
        if (config_.cleanup_interval_ms == 0) {
            config_.cleanup_interval_ms = 1000;
        }
        if (config_.recv_buffer_size == 0) {
            config_.recv_buffer_size = 64 * 1024;
        }
        next_connection_id_.store(config_.first_connection_id == 0 ? 1 : config_.first_connection_id, std::memory_order_relaxed);
        udp_session_.set_read_callback([this](ConnectionContext &context) { on_datagram(context); });
        started_ = udp_session_.bind(config_.host, config_.port, runtime_);
        if (!started_) {
            return false;
        }
        update_timer_ = runtime_.schedule_periodic(config_.update_interval_ms, config_.update_interval_ms, [this] { update_sessions(); });
        if (config_.idle_timeout_ms != 0) {
            cleanup_timer_ = runtime_.schedule_periodic(config_.cleanup_interval_ms, config_.cleanup_interval_ms, [this] { cleanup_idle_sessions(); });
        }
        return true;
    }

    void KcpServerSession::stop()
    {
        if (!started_) {
            return;
        }
        runtime_.cancel_timer(update_timer_);
        runtime_.cancel_timer(cleanup_timer_);
        close_all();
        udp_session_.close();
        started_ = false;
    }

    bool KcpServerSession::send(std::uint64_t connection_id, const std::vector<std::uint8_t> &payload)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto *session = session_for_connection(connection_id);
        if (!session || !session->kcp) {
            return false;
        }
        const auto rc = ikcp_send(session->kcp, reinterpret_cast<const char *>(payload.data()), static_cast<int>(payload.size()));
        if (rc < 0) {
            return false;
        }
        InetAddress output_address = session->address;
        t_kcp_output_address = &output_address;
        ikcp_update(session->kcp, static_cast<IUINT32>(base::time::now()));
        ikcp_flush(session->kcp);
        t_kcp_output_address = nullptr;
        return true;
    }

    bool KcpServerSession::close(std::uint64_t connection_id)
    {
        return erase_session(connection_id, true);
    }

    void KcpServerSession::close_all()
    {
        std::vector<std::uint64_t> ids;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            ids.reserve(conv_by_connection_.size());
            for (const auto &[connection_id, conv] : conv_by_connection_) {
                (void)conv;
                ids.push_back(connection_id);
            }
        }
        for (const auto id : ids) {
            erase_session(id, true);
        }
    }

    std::size_t KcpServerSession::active_connection_count() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return sessions_by_conv_.size();
    }

    KcpServerSession::Metrics KcpServerSession::metrics() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto metrics = metrics_;
        metrics.active_sessions = sessions_by_conv_.size();
        return metrics;
    }

    void KcpServerSession::set_data_callback(DataCallback callback)
    {
        data_callback_ = std::move(callback);
    }

    void KcpServerSession::set_close_callback(CloseCallback callback)
    {
        close_callback_ = std::move(callback);
    }

    void KcpServerSession::set_handshake_validator(HandshakeValidator validator)
    {
        handshake_validator_ = std::move(validator);
    }

    void KcpServerSession::on_datagram(ConnectionContext &context)
    {
        const auto address = context.get_remote_address();
        auto bytes = to_bytes(context.take_input_byte_buffer());
        if (bytes.empty()) {
            return;
        }
        const auto type = bytes.front();
        bytes.erase(bytes.begin());
        if (type == config_.handshake_packet_type) {
            handle_handshake(address, bytes);
        } else if (type == config_.kcp_packet_type) {
            handle_kcp_packet(address, std::move(bytes));
        } else {
            account_malformed(address);
        }
    }

    void KcpServerSession::handle_handshake(const InetAddress &address, const std::vector<std::uint8_t> &payload)
    {
        if (!handshake_rate_allowed(address)) {
            std::lock_guard<std::mutex> lock(mutex_);
            metrics_.handshakes_rate_limited++;
            return;
        }
        const auto decision = handshake_validator_
            ? handshake_validator_(address, payload)
            : HandshakeDecision{config_.handshake_magic.empty() || payload == config_.handshake_magic, 0};
        if (!decision.accepted) {
            std::lock_guard<std::mutex> lock(mutex_);
            metrics_.handshakes_rejected++;
            return;
        }

        if (decision.migrate_conv != 0) {
            if (config_.allow_migration && migrate_session(decision.migrate_conv, address)) {
                std::uint8_t ack[4]{};
                write_u32_be(ack, decision.migrate_conv);
                send_packet(address, config_.handshake_ack_packet_type, ack, sizeof(ack));
                return;
            }
            std::lock_guard<std::mutex> lock(mutex_);
            metrics_.handshakes_rejected++;
            return;
        }

        std::uint32_t conv = 0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (config_.max_sessions != 0 && sessions_by_conv_.size() >= config_.max_sessions) {
                metrics_.handshakes_rejected++;
                return;
            }
            if (too_many_sessions_for_address(address)) {
                metrics_.handshakes_rejected++;
                return;
            }
            if (too_many_sessions_for_ip(address)) {
                metrics_.handshakes_rejected++;
                return;
            }
            const auto key = address_key(address);
            const auto existing = conv_by_address_.find(key);
            if (existing != conv_by_address_.end()) {
                conv = existing->second;
            } else {
                conv = next_conv();
                auto session = std::make_unique<Session>();
                session->conv = conv;
                session->connection_id = next_connection_id_.fetch_add(1, std::memory_order_relaxed);
                session->address = address;
                session->last_activity_ms = base::time::steady_now_ms();
                session->kcp = ikcp_create(conv, this);
                session->kcp->user = this;
                session->kcp->output = &KcpServerSession::kcp_output;
                if (config_.mtu > 0) {
                    (void)ikcp_setmtu(session->kcp, static_cast<int>(config_.mtu));
                }
                ikcp_wndsize(session->kcp, static_cast<int>(config_.send_window), static_cast<int>(config_.receive_window));
                ikcp_nodelay(session->kcp,
                             config_.nodelay ? 1 : 0,
                             static_cast<int>(config_.update_interval_ms),
                             static_cast<int>(config_.resend),
                             config_.no_congestion_control ? 1 : 0);
                conv_by_connection_[session->connection_id] = conv;
                conv_by_address_[key] = conv;
                sessions_by_conv_[conv] = std::move(session);
                metrics_.sessions_created++;
            }
            metrics_.handshakes_accepted++;
        }

        std::uint8_t ack[4]{};
        write_u32_be(ack, conv);
        send_packet(address, config_.handshake_ack_packet_type, ack, sizeof(ack));
    }

    bool KcpServerSession::migrate_session(std::uint32_t conv, const InetAddress &address)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto *session = session_for_conv(conv);
        if (!session) {
            return false;
        }
        const auto new_key = address_key(address);
        const auto existing = conv_by_address_.find(new_key);
        if (existing != conv_by_address_.end() && existing->second != conv) {
            return false;
        }
        conv_by_address_.erase(address_key(session->address));
        session->address = address;
        session->last_activity_ms = base::time::steady_now_ms();
        conv_by_address_[new_key] = conv;
        metrics_.handshakes_accepted++;
        return true;
    }

    void KcpServerSession::handle_kcp_packet(const InetAddress &address, std::vector<std::uint8_t> packet)
    {
        if (packet.size() < 4) {
            account_malformed(address);
            return;
        }
        const auto conv = ikcp_getconv(packet.data());
        std::uint64_t connection_id = 0;
        std::vector<std::vector<std::uint8_t>> payloads;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto *session = session_for_conv(conv);
            if (!session || session->address != address) {
                metrics_.malformed_packets++;
                return;
            }
            connection_id = session->connection_id;
            session->last_activity_ms = base::time::steady_now_ms();
            InetAddress output_address = session->address;
            t_kcp_output_address = &output_address;
            if (ikcp_input(session->kcp, reinterpret_cast<const char *>(packet.data()), static_cast<long>(packet.size())) < 0) {
                t_kcp_output_address = nullptr;
                metrics_.kcp_input_errors++;
                runtime_.dispatch([this, connection_id] { erase_session(connection_id, true); });
                return;
            }
            std::vector<char> buffer(config_.recv_buffer_size);
            for (;;) {
                const auto received = ikcp_recv(session->kcp, buffer.data(), static_cast<int>(buffer.size()));
                if (received <= 0) {
                    break;
                }
                payloads.emplace_back(buffer.begin(), buffer.begin() + received);
                metrics_.payloads_received++;
            }
            t_kcp_output_address = nullptr;
        }

        for (auto &payload : payloads) {
            if (data_callback_) {
                data_callback_(connection_id, std::move(payload));
            }
        }
    }

    void KcpServerSession::update_sessions()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto now = base::time::now();
        for (auto &[conv, session] : sessions_by_conv_) {
            (void)conv;
            if (session && session->kcp) {
                InetAddress output_address = session->address;
                t_kcp_output_address = &output_address;
                ikcp_update(session->kcp, static_cast<IUINT32>(now));
                t_kcp_output_address = nullptr;
            }
        }
    }

    void KcpServerSession::cleanup_idle_sessions()
    {
        const auto now = base::time::steady_now_ms();
        std::vector<std::uint64_t> expired;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (const auto &[conv, session] : sessions_by_conv_) {
                (void)conv;
                if (session && session->last_activity_ms + config_.idle_timeout_ms <= now) {
                    expired.push_back(session->connection_id);
                }
            }
        }
        for (const auto id : expired) {
            erase_session(id, true);
        }
    }

    bool KcpServerSession::erase_session(std::uint64_t connection_id, bool notify)
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto conn_it = conv_by_connection_.find(connection_id);
            if (conn_it == conv_by_connection_.end()) {
                return false;
            }
            const auto conv = conn_it->second;
            conv_by_connection_.erase(conn_it);
            const auto session_it = sessions_by_conv_.find(conv);
            if (session_it != sessions_by_conv_.end()) {
                conv_by_address_.erase(address_key(session_it->second->address));
                if (session_it->second->kcp) {
                    ikcp_release(session_it->second->kcp);
                    session_it->second->kcp = nullptr;
                }
                sessions_by_conv_.erase(session_it);
                metrics_.sessions_closed++;
            }
        }
        if (notify && close_callback_) {
            close_callback_(connection_id);
        }
        return true;
    }

    void KcpServerSession::send_packet(const InetAddress &address, std::uint8_t type, const void *data, std::size_t size)
    {
        const auto *first = static_cast<const std::uint8_t *>(data);
        std::vector<std::uint8_t> bytes;
        if (first && size > 0) {
            bytes.assign(first, first + size);
        }
        udp_session_.dispatch([this, address, type, bytes = std::move(bytes)] {
            auto buffer = to_buffer(type, bytes.data(), bytes.size());
            (void)udp_session_.send_datagram(address, buffer);
        });
    }

    std::uint32_t KcpServerSession::next_conv()
    {
        for (;;) {
            const auto conv = next_conv_.fetch_add(1, std::memory_order_relaxed);
            if (conv != 0 && sessions_by_conv_.find(conv) == sessions_by_conv_.end()) {
                return conv;
            }
        }
    }

    std::string KcpServerSession::address_key(const InetAddress &address) const
    {
        return address.to_address_key();
    }

    KcpServerSession::Session *KcpServerSession::session_for_connection(std::uint64_t connection_id)
    {
        const auto conn_it = conv_by_connection_.find(connection_id);
        if (conn_it == conv_by_connection_.end()) {
            return nullptr;
        }
        return session_for_conv(conn_it->second);
    }

    KcpServerSession::Session *KcpServerSession::session_for_conv(std::uint32_t conv)
    {
        const auto it = sessions_by_conv_.find(conv);
        return it == sessions_by_conv_.end() ? nullptr : it->second.get();
    }

    int KcpServerSession::kcp_output(const char *buf, int len, ikcpcb *kcp, void *user)
    {
        auto *self = static_cast<KcpServerSession *>(user ? user : (kcp ? kcp->user : nullptr));
        if (!self || !t_kcp_output_address || len <= 0) {
            return -1;
        }
        self->send_packet(*t_kcp_output_address, self->config_.kcp_packet_type, buf, static_cast<std::size_t>(len));
        return len;
    }

    bool KcpServerSession::handshake_rate_allowed(const InetAddress &address)
    {
        if (config_.max_handshakes_per_address_per_window == 0) {
            return true;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        const auto now = base::time::steady_now_ms();
        auto &state = address_rates_[address_key(address)];
        if (state.window_start_ms == 0 || state.window_start_ms + config_.handshake_rate_window_ms <= now) {
            state.window_start_ms = now;
            state.handshakes = 0;
        }
        if (state.handshakes >= config_.max_handshakes_per_address_per_window) {
            return false;
        }
        state.handshakes++;
        return true;
    }

    bool KcpServerSession::too_many_sessions_for_address(const InetAddress &address) const
    {
        if (config_.max_sessions_per_address == 0) {
            return false;
        }
        return conv_by_address_.find(address_key(address)) != conv_by_address_.end() && config_.max_sessions_per_address <= 1;
    }

    bool KcpServerSession::too_many_sessions_for_ip(const InetAddress &address) const
    {
        if (config_.max_sessions_per_ip == 0) {
            return false;
        }
        std::size_t count = 0;
        for (const auto &[conv, session] : sessions_by_conv_) {
            (void)conv;
            if (session && session->address.get_ip() == address.get_ip()) {
                ++count;
            }
        }
        return count >= config_.max_sessions_per_ip;
    }

    void KcpServerSession::account_malformed(const InetAddress &address)
    {
        std::uint64_t close_connection_id = 0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            metrics_.malformed_packets++;
            if (config_.max_malformed_packets_per_address == 0) {
                return;
            }
            auto &state = address_rates_[address_key(address)];
            state.malformed++;
            if (state.malformed < config_.max_malformed_packets_per_address) {
                return;
            }
            const auto it = conv_by_address_.find(address_key(address));
            if (it != conv_by_address_.end()) {
                auto *session = session_for_conv(it->second);
                close_connection_id = session ? session->connection_id : 0;
            }
        }
        if (close_connection_id != 0) {
            erase_session(close_connection_id, true);
        }
    }
}
