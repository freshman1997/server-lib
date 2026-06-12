#ifndef __NET_SSH_CONNECTION_SSH_CONNECTION_MANAGER_H__
#define __NET_SSH_CONNECTION_SSH_CONNECTION_MANAGER_H__

#include "connection/ssh_channel.h"
#include "connection/ssh_port_forwarding.h"
#include "protocol/ssh_constants.h"
#include "protocol/ssh_structures.h"
#include "ssh_channel_handler.h"
#include "buffer/byte_buffer.h"
#include "coroutine/runtime_view.h"
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace yuan::net::ssh
{
    using ::yuan::buffer::ByteBuffer;

    class SshSession;
    class SshHandler;

    class SshConnectionManager
    {
    public:
        using SubsystemFactory = std::function<std::unique_ptr<SshChannelHandler>()>;

        explicit SshConnectionManager(SshSession *session, uint32_t max_channels = SSH_MAX_CHANNELS_PER_SESSION);

        void register_subsystem(const std::string &name, SubsystemFactory factory);

        SshChannel *create_channel(const std::string &type, uint32_t remote_channel_id,
                                   uint32_t initial_window, uint32_t max_packet_size);
        SshChannel *find_channel(uint32_t local_id);
        SshChannel *find_channel_by_remote(uint32_t remote_id);
        void remove_channel(uint32_t local_id);
        void close_all_channels();

        uint32_t next_channel_id();

        uint32_t channel_count() const;
        bool channel_limit_reached() const;

        ByteBuffer handle_channel_open(const SshChannelOpenMessage &msg, SshHandler *handler);
        void handle_channel_open_confirmation(const SshChannelOpenConfirmationMessage &msg);
        void handle_channel_open_failure(const SshChannelOpenFailureMessage &msg);
        ByteBuffer handle_channel_data(const SshChannelDataMessage &msg, SshHandler *handler);
        ByteBuffer handle_channel_extended_data(const SshChannelExtendedDataMessage &msg);
        ByteBuffer handle_channel_window_adjust(const SshChannelWindowAdjustMessage &msg);
        ByteBuffer handle_channel_eof(const SshChannelEofMessage &msg);
        ByteBuffer handle_channel_close(const SshChannelCloseMessage &msg, SshHandler *handler);
        ByteBuffer handle_channel_request(const SshChannelRequestMessage &msg, SshHandler *handler);
        ByteBuffer handle_global_request(const SshGlobalRequestMessage &msg, SshHandler *handler);

        ByteBuffer build_channel_open_confirmation(uint32_t recipient, uint32_t sender,
                                                   uint32_t window, uint32_t max_pkt) const;
        ByteBuffer build_channel_open_failure(uint32_t recipient,
                                              SshChannelOpenFailureReason reason,
                                              const std::string &description) const;
        ByteBuffer build_channel_data(uint32_t recipient, const std::vector<uint8_t> &data) const;
        ByteBuffer build_channel_extended_data(uint32_t recipient, uint32_t type,
                                               const std::vector<uint8_t> &data) const;
        ByteBuffer build_channel_eof(uint32_t recipient) const;
        ByteBuffer build_channel_close(uint32_t recipient) const;
        ByteBuffer build_window_adjust(uint32_t recipient, uint32_t bytes) const;
        ByteBuffer build_channel_success(uint32_t recipient) const;
        ByteBuffer build_channel_failure(uint32_t recipient) const;
        ByteBuffer build_channel_exit_status(uint32_t recipient, uint32_t exit_status) const;
        ByteBuffer build_channel_exit_signal(uint32_t recipient,
                                             const std::string &signal_name,
                                             bool core_dumped,
                                             const std::string &error_message,
                                             const std::string &language_tag) const;

        ByteBuffer build_request_success(const std::vector<uint8_t> &data = {}) const;
        ByteBuffer build_request_failure() const;

        ByteBuffer build_forwarded_tcpip_channel_open(const std::string &connected_address,
                                                      uint32_t connected_port,
                                                      const std::string &originator_address,
                                                      uint32_t originator_port,
                                                      uint32_t initial_window = SSH_DEFAULT_WINDOW_SIZE,
                                                      uint32_t maximum_packet_size = SSH_DEFAULT_MAX_PACKET_SIZE);

        std::optional<uint32_t> open_forwarded_tcpip_channel(const std::string &connected_address,
                                                             uint32_t connected_port,
                                                             const std::string &originator_address,
                                                             uint32_t originator_port,
                                                             ByteBuffer &packet_out,
                                                             uint32_t initial_window = SSH_DEFAULT_WINDOW_SIZE,
                                                             uint32_t maximum_packet_size = SSH_DEFAULT_MAX_PACKET_SIZE);

        bool register_forwarded_tcpip_handler(uint32_t local_channel_id,
                                              std::unique_ptr<SshChannelHandler> handler);

        void set_runtime(coroutine::RuntimeView runtime);
        void poll_async_tasks();
        bool has_remote_forwards() const;

        std::vector<ByteBuffer> drain_channel_pending_data();

    private:
        ByteBuffer maybe_adjust_window(SshChannel *channel);

        SshSession *session_;
        uint32_t max_channels_;
        std::atomic<uint32_t> next_local_id_{ 0 };

        mutable std::mutex channels_mutex_;
        std::unordered_map<uint32_t, std::unique_ptr<SshChannel> > channels_;

        SshPortForwarding port_forwarding_;
        std::unordered_map<std::string, SubsystemFactory> subsystem_factories_;
    };
}

#endif
