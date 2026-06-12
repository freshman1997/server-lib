#ifndef YUAN_RELEASE_SSH_CLI_FORWARD_RUNTIME_H
#define YUAN_RELEASE_SSH_CLI_FORWARD_RUNTIME_H

#include "ssh_cli_config.h"
#include "ssh_cli_forward.h"
#include "ssh_cli_socket.h"
#include "protocol/ssh_message_codec.h"
#include "transport/ssh_transport.h"

#include <cstdint>
#include <functional>
#include <iosfwd>
#include <string>
#include <unordered_map>
#include <vector>

namespace yuan::release_ssh::client
{
    class SshCliForwardRuntime
    {
    public:
        struct PendingSocksClient
        {
            SocketHandle socket = kInvalidSocket;
            std::string origin_host;
            uint16_t origin_port = 0;
            std::vector<uint8_t> recv_buf;
            bool method_negotiated = false;
        };

        ~SshCliForwardRuntime();

        SshCliForwardRuntime() = default;
        SshCliForwardRuntime(const SshCliForwardRuntime &) = delete;
        SshCliForwardRuntime &operator=(const SshCliForwardRuntime &) = delete;

        bool configure(const CliArgs &args, std::ostream &err);
        void cleanup();
        void close_channel(uint32_t local_id);
        bool has_forwarding() const;
        bool has_activity() const;
        bool send_socks_reply(SocketHandle sock, uint8_t rep_code);
        bool pump_local_forward_accepts(SocketHandle ssh_fd,
                                        yuan::net::ssh::SshTransport &transport,
                                        bool auth_ok,
                                        const std::function<void(const std::string &)> &debug,
                                        std::ostream &err);
        bool pump_dynamic_forward_accepts(bool auth_ok,
                                          const std::function<void(const std::string &)> &debug,
                                          std::ostream &err);
        bool pump_dynamic_socks_handshake(SocketHandle ssh_fd,
                                          yuan::net::ssh::SshTransport &transport,
                                          const std::function<void(const std::string &)> &debug);
        bool pump_forward_target_reads(SocketHandle ssh_fd,
                                       yuan::net::ssh::SshTransport &transport,
                                       const std::function<void(const std::string &)> &debug);
        bool send_remote_forward_requests(SocketHandle ssh_fd,
                                          yuan::net::ssh::SshTransport &transport);
        void send_remote_forward_cancel_requests(SocketHandle ssh_fd,
                                                 yuan::net::ssh::SshTransport &transport);
        bool handle_open_confirmation(const yuan::net::ssh::SshChannelOpenConfirmationMessage &conf,
                                       const std::function<void(const std::string &)> &debug,
                                       bool &handled);
        bool handle_open_failure(const yuan::net::ssh::SshChannelOpenFailureMessage &failure,
                                 const std::function<void(const std::string &)> &debug,
                                 bool &handled);
        bool handle_request_success();
        bool handle_request_failure(std::ostream &err);
        bool handle_forwarded_tcpip_open(SocketHandle ssh_fd,
                                         yuan::net::ssh::SshTransport &transport,
                                         const yuan::net::ssh::SshChannelOpenMessage &open,
                                         const std::function<void(const std::string &)> &debug,
                                         bool &handled);
        bool handle_channel_data(SocketHandle ssh_fd,
                                 yuan::net::ssh::SshTransport &transport,
                                 const yuan::net::ssh::SshChannelDataMessage &data_msg,
                                 const std::function<void(const std::string &)> &debug,
                                 bool &handled);
        bool handle_channel_eof(const yuan::net::ssh::SshChannelEofMessage &eof_msg);
        bool handle_channel_close(SocketHandle ssh_fd,
                                  yuan::net::ssh::SshTransport &transport,
                                  const yuan::net::ssh::SshChannelCloseMessage &close_msg,
                                  uint32_t session_local_channel_id,
                                  bool &handled);

        std::vector<LocalForwardSpec> local_forward_specs;
        std::vector<DynamicForwardSpec> dynamic_forward_specs;
        std::vector<RemoteForwardSpec> remote_forward_specs;
        std::vector<RemoteForwardSpec> pending_remote_forward_specs;
        uint32_t next_forward_local_channel_id = 1024;
        std::unordered_map<std::string, SocketHandle> local_forward_listeners;
        std::unordered_map<std::string, SocketHandle> dynamic_forward_listeners;
        std::unordered_map<uint32_t, SocketHandle> pending_local_open_socket;
        std::unordered_map<uint32_t, SocketHandle> pending_dynamic_open_socket;
        std::unordered_map<uint32_t, SocketHandle> forward_local_to_socket;
        std::unordered_map<uint32_t, uint32_t> forward_local_to_remote;
        std::unordered_map<uint32_t, uint32_t> forward_remote_to_local;
        std::unordered_map<SocketHandle, PendingSocksClient> pending_socks_clients;

    private:
        void close_socks_client(SocketHandle sock);
    };
}

#endif
