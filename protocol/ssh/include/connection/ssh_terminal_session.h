#ifndef __NET_SSH_CONNECTION_SSH_TERMINAL_SESSION_H__
#define __NET_SSH_CONNECTION_SSH_TERMINAL_SESSION_H__

#include <cstdint>
#include <string>
#include <vector>

namespace yuan::net::ssh
{
    struct SshTerminalSpec
    {
        std::string term_env;
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t pixel_width = 0;
        uint32_t pixel_height = 0;
        std::vector<uint8_t> terminal_modes;
    };

    struct SshTerminalSessionState
    {
        bool has_pty_request = false;
        bool interactive_shell_requested = false;
        bool exec_requested = false;
        bool subsystem_requested = false;
        bool pty_bridge_active = false;
        std::string exec_command;
        SshTerminalSpec spec;
    };
}

#endif
