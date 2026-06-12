#include "ssh_cli_session.h"

#include "ssh_cli_messages.h"

#include "protocol/ssh_message_codec.h"

namespace yuan::release_ssh::client
{
    bool send_userauth_service_request(SocketHandle fd,
                                       yuan::net::ssh::SshTransport &transport)
    {
        using namespace yuan::net::ssh;

        SshServiceRequestMessage service_req;
        service_req.service_name = SSH_SERVICE_USERAUTH;
        return send_packet(fd, transport, SshMessageCodec::encode_service_request(service_req));
    }

    bool send_password_auth_request(SocketHandle fd,
                                    yuan::net::ssh::SshTransport &transport,
                                    const std::string &username,
                                    const std::string &password)
    {
        using namespace yuan::net::ssh;

        SshUserauthRequestMessage auth_req;
        auth_req.username = username;
        auth_req.service_name = SSH_SERVICE_CONNECTION;
        auth_req.method_name = "password";
        auth_req.method_specific_data = make_password_method_data(password);
        return send_packet(fd, transport, SshMessageCodec::encode_userauth_request(auth_req));
    }

    bool send_publickey_probe_request(SocketHandle fd,
                                      yuan::net::ssh::SshTransport &transport,
                                      const std::string &username,
                                      const ClientIdentity &identity)
    {
        using namespace yuan::net::ssh;

        SshUserauthRequestMessage auth_req;
        auth_req.username = username;
        auth_req.service_name = SSH_SERVICE_CONNECTION;
        auth_req.method_name = "publickey";
        auth_req.method_specific_data = make_publickey_method_data(
            identity.algorithm,
            identity.public_key_blob,
            false,
            {});
        return send_packet(fd, transport, SshMessageCodec::encode_userauth_request(auth_req));
    }

    bool send_signed_publickey_auth_request(SocketHandle fd,
                                            yuan::net::ssh::SshTransport &transport,
                                            const std::string &username,
                                            const ClientIdentity &identity,
                                            const std::vector<uint8_t> &signature)
    {
        using namespace yuan::net::ssh;

        SshUserauthRequestMessage auth_req;
        auth_req.username = username;
        auth_req.service_name = SSH_SERVICE_CONNECTION;
        auth_req.method_name = "publickey";
        auth_req.method_specific_data = make_publickey_method_data(
            identity.algorithm,
            identity.public_key_blob,
            true,
            signature);
        return send_packet(fd, transport, SshMessageCodec::encode_userauth_request(auth_req));
    }

    bool send_session_channel_open(SocketHandle fd,
                                   yuan::net::ssh::SshTransport &transport,
                                   uint32_t local_channel_id)
    {
        using namespace yuan::net::ssh;

        SshChannelOpenMessage open_msg;
        open_msg.channel_type = SSH_CHANNEL_SESSION;
        open_msg.sender_channel = local_channel_id;
        open_msg.initial_window_size = SSH_DEFAULT_WINDOW_SIZE;
        open_msg.maximum_packet_size = SSH_DEFAULT_MAX_PACKET_SIZE;
        return send_packet(fd, transport, SshMessageCodec::encode_channel_open(open_msg));
    }

    bool send_exec_request(SocketHandle fd,
                           yuan::net::ssh::SshTransport &transport,
                           uint32_t remote_channel_id,
                           const std::string &command)
    {
        using namespace yuan::net::ssh;

        SshChannelRequestMessage req;
        req.recipient_channel = remote_channel_id;
        req.request_type = "exec";
        req.want_reply = true;
        req.request_specific_data = make_exec_request_data(command);
        return send_packet(fd, transport, SshMessageCodec::encode_channel_request(req));
    }

    bool send_pty_request(SocketHandle fd,
                          yuan::net::ssh::SshTransport &transport,
                          uint32_t remote_channel_id,
                          const TerminalSize &terminal_size)
    {
        using namespace yuan::net::ssh;

        SshChannelRequestMessage req;
        req.recipient_channel = remote_channel_id;
        req.request_type = "pty-req";
        req.want_reply = true;
        req.request_specific_data = make_pty_request_data(
            terminal_size.cols,
            terminal_size.rows,
            terminal_size.pixel_width,
            terminal_size.pixel_height);
        return send_packet(fd, transport, SshMessageCodec::encode_channel_request(req));
    }

    bool send_shell_request(SocketHandle fd,
                            yuan::net::ssh::SshTransport &transport,
                            uint32_t remote_channel_id)
    {
        using namespace yuan::net::ssh;

        SshChannelRequestMessage req;
        req.recipient_channel = remote_channel_id;
        req.request_type = "shell";
        req.want_reply = true;
        return send_packet(fd, transport, SshMessageCodec::encode_channel_request(req));
    }

    bool send_signal_request(SocketHandle fd,
                             yuan::net::ssh::SshTransport &transport,
                             uint32_t remote_channel_id,
                             const std::string &signal_name)
    {
        using namespace yuan::net::ssh;

        SshChannelRequestMessage req;
        req.recipient_channel = remote_channel_id;
        req.request_type = "signal";
        req.want_reply = false;
        req.request_specific_data = make_signal_request_data(signal_name);
        return send_packet(fd, transport, SshMessageCodec::encode_channel_request(req));
    }

    bool send_window_change_request(SocketHandle fd,
                                    yuan::net::ssh::SshTransport &transport,
                                    uint32_t remote_channel_id,
                                    const TerminalSize &terminal_size)
    {
        using namespace yuan::net::ssh;

        SshChannelRequestMessage req;
        req.recipient_channel = remote_channel_id;
        req.request_type = "window-change";
        req.want_reply = false;
        req.request_specific_data = make_window_change_request_data(
            terminal_size.cols,
            terminal_size.rows,
            terminal_size.pixel_width,
            terminal_size.pixel_height);
        return send_packet(fd, transport, SshMessageCodec::encode_channel_request(req));
    }
}
