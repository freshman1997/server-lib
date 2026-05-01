#ifndef __NET_SSH_CONNECTION_SSH_PTY_PROCESS_H__
#define __NET_SSH_CONNECTION_SSH_PTY_PROCESS_H__

#include "connection/ssh_terminal_session.h"
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace yuan::net::ssh
{
    struct SshPtyExitState
    {
        bool exited = false;
        bool signaled = false;
        int exit_code = -1;
        int term_signal = 0;
    };

    class SshPtyBackend
    {
    public:
        SshPtyBackend() = default;
        ~SshPtyBackend();

        bool allocate(const SshTerminalSpec &spec, std::string *error_message = nullptr);
        void release();

        bool allocated() const;
        int master_fd() const;
        int slave_fd() const;

    private:
        int master_fd_ = -1;
        int slave_fd_ = -1;
    };

    class SshPtyProcess
    {
    public:
        SshPtyProcess() = default;
        ~SshPtyProcess();

        bool prepare(const SshTerminalSpec &spec, std::string *error_message = nullptr);
        bool launch_shell(const std::string &command,
                          bool interactive,
                          std::string *error_message = nullptr);
        bool write_input(const uint8_t *data, size_t len, size_t *written = nullptr);
        bool read_output(std::vector<uint8_t> *out, size_t max_bytes = 64 * 1024);
        bool resize_terminal(uint32_t width,
                             uint32_t height,
                             uint32_t pixel_width,
                             uint32_t pixel_height,
                             std::string *error_message = nullptr);
        bool send_signal(const std::string &signal_name,
                         std::string *error_message = nullptr);
        bool poll_exit(SshPtyExitState *state);
        void shutdown();

        bool ready() const;
        const SshPtyBackend &backend() const;

    private:
        SshPtyBackend backend_;
        int child_pid_ = -1;
        SshTerminalSpec spec_;
    };
}

#endif
