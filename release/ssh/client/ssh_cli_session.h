#ifndef YUAN_RELEASE_SSH_CLI_SESSION_H
#define YUAN_RELEASE_SSH_CLI_SESSION_H

#include "ssh_cli_auth.h"
#include "ssh_cli_socket.h"
#include "ssh_cli_terminal.h"

#include "transport/ssh_transport.h"

#include <cstdint>
#include <string>
#include <vector>

namespace yuan::release_ssh::client
{
    bool send_userauth_service_request(SocketHandle fd,
                                       yuan::net::ssh::SshTransport &transport);
    bool send_password_auth_request(SocketHandle fd,
                                    yuan::net::ssh::SshTransport &transport,
                                    const std::string &username,
                                    const std::string &password);
    bool send_publickey_probe_request(SocketHandle fd,
                                      yuan::net::ssh::SshTransport &transport,
                                      const std::string &username,
                                      const ClientIdentity &identity);
    bool send_signed_publickey_auth_request(SocketHandle fd,
                                            yuan::net::ssh::SshTransport &transport,
                                            const std::string &username,
                                            const ClientIdentity &identity,
                                            const std::vector<uint8_t> &signature);
    bool send_session_channel_open(SocketHandle fd,
                                   yuan::net::ssh::SshTransport &transport,
                                   uint32_t local_channel_id);
    bool send_exec_request(SocketHandle fd,
                           yuan::net::ssh::SshTransport &transport,
                           uint32_t remote_channel_id,
                           const std::string &command);
    bool send_pty_request(SocketHandle fd,
                          yuan::net::ssh::SshTransport &transport,
                          uint32_t remote_channel_id,
                          const TerminalSize &terminal_size);
    bool send_shell_request(SocketHandle fd,
                            yuan::net::ssh::SshTransport &transport,
                            uint32_t remote_channel_id);
    bool send_signal_request(SocketHandle fd,
                             yuan::net::ssh::SshTransport &transport,
                             uint32_t remote_channel_id,
                             const std::string &signal_name);
    bool send_window_change_request(SocketHandle fd,
                                    yuan::net::ssh::SshTransport &transport,
                                    uint32_t remote_channel_id,
                                    const TerminalSize &terminal_size);
}

#endif
