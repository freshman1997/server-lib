#ifndef __NET_SSH_CONNECTION_SSH_TERMINAL_BRIDGE_H__
#define __NET_SSH_CONNECTION_SSH_TERMINAL_BRIDGE_H__

#include "buffer/byte_buffer.h"
#include "connection/ssh_channel.h"
#include "connection/ssh_pty_process.h"
#include "connection/ssh_connection_manager.h"
#include "ssh_handler.h"
#include <memory>
#include <mutex>
#include <vector>
#include <unordered_map>
#include <unordered_set>

namespace yuan::net::ssh
{
    class SshSession;
    class SshNonPtyShellProcess;
    class SshTerminalBridge
    {
    public:
        explicit SshTerminalBridge(SshSession *session,
                                   SshConnectionManager *conn_mgr);
        ~SshTerminalBridge();

        void register_pty_process(uint32_t channel_remote_id, std::unique_ptr<SshPtyProcess> process);
        bool has_pty_process(uint32_t channel_remote_id) const;
        bool has_any_pty_processes() const;
        bool has_any_client_pty_processes() const;
        int first_pty_master_fd() const;
        std::vector<int> terminal_output_fds() const;
        bool pump_pty_once(uint32_t channel_remote_id, SshHandler *handler);
        bool pump_all_pty_once(SshHandler *handler);
        void shutdown_pty_for_channel(uint32_t channel_remote_id);
        void shutdown_all_pty_processes();

        void handle_channel_data(const SshChannelDataMessage &msg, SshHandler *handler);
        void handle_channel_eof(const SshChannelEofMessage &msg);
        void handle_channel_request(const SshChannelRequestMessage &msg,
                                    const yuan::buffer::ByteBuffer &response,
                                    SshHandler *handler);

    private:
        void on_pty_window_change_request(const SshChannelRequestMessage &msg);
        void on_pty_signal_request(const SshChannelRequestMessage &msg);
        void maybe_start_shell_pty_bridge(const SshChannelRequestMessage &msg,
                                          const yuan::buffer::ByteBuffer &response,
                                          SshHandler *handler);
        void maybe_start_shell_pipe_bridge(const SshChannelRequestMessage &msg,
                                           const yuan::buffer::ByteBuffer &response);
        void maybe_start_exec_pty_bridge(const SshChannelRequestMessage &msg,
                                         const yuan::buffer::ByteBuffer &response,
                                         SshHandler *handler);

        SshSession *session_;
        SshConnectionManager *conn_mgr_;
        mutable std::mutex pty_mutex_;
        std::unordered_map<uint32_t, std::unique_ptr<SshPtyProcess> > pty_processes_;
        std::unordered_map<uint32_t, std::unique_ptr<SshNonPtyShellProcess> > shell_processes_;
        std::unordered_set<uint32_t> channels_with_input_since_last_pump_;
    };
}

#endif
