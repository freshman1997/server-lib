#ifdef _WIN32
#include <winsock2.h>
#endif

#include "client/ssh_cli_auth.h"
#include "client/ssh_cli_config.h"
#include "client/ssh_cli_forward.h"
#include "client/ssh_cli_forward_runtime.h"
#include "client/ssh_cli_messages.h"
#include "client/ssh_cli_session.h"
#include "client/ssh_cli_socket.h"
#include "client/ssh_cli_terminal.h"
#include "client/ssh_cli_transport.h"

#include "algorithm/ssh_algorithm_registry.h"
#include "crypto/ssh_crypto_openssl.h"
#include "protocol/ssh_message_codec.h"
#include "transport/ssh_transport.h"
#include "transport/ssh_version_exchange.h"
#include "platform/native_platform.h"

#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#ifndef _WIN32
#include <signal.h>
#endif


namespace
{
#ifndef _WIN32
    volatile sig_atomic_t g_cli_sigint_count = 0;

    void cli_sigint_handler(int)
    {
        g_cli_sigint_count = 1;
    }
#endif

    using yuan::release_ssh::client::ClientIdentity;
    using yuan::release_ssh::client::CliArgs;
    using yuan::release_ssh::client::connect_tcp;
    using yuan::release_ssh::client::encode_channel_data_packet;
    using yuan::release_ssh::client::list_contains;
    using yuan::release_ssh::client::load_client_identity;
    using yuan::release_ssh::client::LocalTerminalRawGuard;
    using yuan::release_ssh::client::make_publickey_signature;
    using yuan::release_ssh::client::parse_args;
    using yuan::release_ssh::client::PacketReadStatus;
    using yuan::release_ssh::client::perform_client_key_exchange;
    using yuan::release_ssh::client::print_usage;
    using yuan::release_ssh::client::query_terminal_size;
    using yuan::release_ssh::client::read_packet;
    using yuan::release_ssh::client::read_stdin_chunk_nonblocking;
    using yuan::release_ssh::client::recv_line;
    using yuan::release_ssh::client::RecvStatus;
    using yuan::release_ssh::client::run_version_probe;
    using yuan::release_ssh::client::send_exec_request;
    using yuan::release_ssh::client::send_password_auth_request;
    using yuan::release_ssh::client::send_pty_request;
    using yuan::release_ssh::client::send_publickey_probe_request;
    using yuan::release_ssh::client::send_session_channel_open;
    using yuan::release_ssh::client::send_shell_request;
    using yuan::release_ssh::client::send_signal_request;
    using yuan::release_ssh::client::send_signed_publickey_auth_request;
    using yuan::release_ssh::client::send_userauth_service_request;
    using yuan::release_ssh::client::send_window_change_request;
    using yuan::release_ssh::client::send_all;
    using yuan::release_ssh::client::send_packet;
    using yuan::release_ssh::client::set_recv_timeout;
    using yuan::release_ssh::client::split_name_list;
    using yuan::release_ssh::client::SocketGuard;
    using yuan::release_ssh::client::SocketHandle;
    using yuan::release_ssh::client::SshCliForwardRuntime;
    using yuan::release_ssh::client::StdinNonblockingGuard;
    using yuan::release_ssh::client::StdinPollResult;
    using yuan::release_ssh::client::stdin_is_tty;
    using yuan::release_ssh::client::TerminalSize;
    using yuan::release_ssh::client::wait_client_io;

    bool run_exec_phase1(SocketHandle fd, const CliArgs &args)
    {
        using namespace yuan::net::ssh;

        bool stderr_line_start = true;
        auto write_stderr = [&](const std::vector<uint8_t> &data) {
            if (data.empty()) {
                return;
            }
            if (!args.stderr_prefix) {
                std::cerr.write(reinterpret_cast<const char *>(data.data()),
                                static_cast<std::streamsize>(data.size()));
                std::cerr.flush();
                return;
            }

            constexpr const char *kPrefix = "[stderr] ";
            for (uint8_t byte : data) {
                if (stderr_line_start) {
                    std::cerr << kPrefix;
                    stderr_line_start = false;
                }
                std::cerr.put(static_cast<char>(byte));
                if (byte == '\n') {
                    stderr_line_start = true;
                }
            }
            std::cerr.flush();
        };

        StdinNonblockingGuard stdin_guard;

        SshCryptoOpenSSL crypto;
        SshAlgorithmRegistry registry;
        registry.register_defaults();

        SshTransport transport(&registry, &crypto, false);
        transport.set_we_are_server(false);

        auto debug = [&](const std::string &message) {
            if (args.verbose > 0) {
                std::cerr << "debug: " << message << '\n';
            }
        };

        yuan::buffer::ByteBuffer recv_buf;
        if (!perform_client_key_exchange(fd, args, transport, registry, crypto, recv_buf, debug)) {
            return false;
        }

        if (!send_userauth_service_request(fd, transport)) {
            std::cerr << "failed to send service request\n";
            return false;
        }
        debug("sent service request");

        const bool shell_mode = args.command.empty();
#ifdef _WIN32
        const bool allocate_pty = shell_mode;
#else
        const bool allocate_pty = shell_mode && stdin_is_tty();
#endif
        bool service_accepted = false;
        bool auth_ok = false;
        bool attempted_publickey = false;
        bool waiting_pk_ok = false;
        ClientIdentity identity;
        if (!load_client_identity(args, identity, std::cerr)) {
            return false;
        }
        bool channel_opened = false;
        bool exec_sent = false;
        bool shell_sent = false;
        bool pty_sent = false;
        bool pty_ok = false;
        bool stdin_eof_sent = false;
        bool shell_ready = false;
        TerminalSize last_terminal_size = query_terminal_size();
#ifndef _WIN32
        sig_atomic_t handled_sigint_count = 0;
#endif
        uint32_t local_channel_id = 0;
        uint32_t remote_channel_id = 0;
        uint32_t exit_code = 0;
        bool got_exit_status = false;
        bool session_channel_closed = false;
        SshCliForwardRuntime forwarding;
        if (!forwarding.configure(args, std::cerr)) {
            return false;
        }
        const bool has_forwarding = forwarding.has_forwarding();
        {
            int io_poll_timeout_ms = args.timeout_ms;
            const bool need_responsive_poll =
                shell_mode ||
                has_forwarding;
            if (need_responsive_poll && io_poll_timeout_ms > 100) {
                io_poll_timeout_ms = 100;
            }
            if (io_poll_timeout_ms < 100) {
                io_poll_timeout_ms = 100;
            }
            (void)set_recv_timeout(fd, io_poll_timeout_ms);
        }

        LocalTerminalRawGuard local_terminal_raw;

        auto cleanup_forward_resources = [&]() {
            forwarding.cleanup();
        };

        auto has_forward_activity = [&]() -> bool {
            return forwarding.has_activity();
        };

        auto pump_local_forward_accepts = [&]() -> bool {
            return forwarding.pump_local_forward_accepts(fd, transport, auth_ok, debug, std::cerr);
        };

        auto pump_dynamic_forward_accepts = [&]() -> bool {
            return forwarding.pump_dynamic_forward_accepts(auth_ok, debug, std::cerr);
        };

        auto pump_dynamic_socks_handshake = [&]() -> bool {
            return forwarding.pump_dynamic_socks_handshake(fd, transport, debug);
        };

        auto pump_forward_target_reads = [&]() -> bool {
            return forwarding.pump_forward_target_reads(fd, transport, debug);
        };

        auto pump_stdin_once = [&]() -> bool {
            if (!shell_mode || !shell_ready || stdin_eof_sent) {
                return true;
            }
            std::string stdin_chunk;
            const auto poll = read_stdin_chunk_nonblocking(stdin_chunk);
            if (poll == StdinPollResult::data && !stdin_chunk.empty()) {
                auto ch_data = encode_channel_data_packet(remote_channel_id, stdin_chunk);
                if (!send_packet(fd, transport, ch_data)) {
                    std::cerr << "failed to send interactive input\n";
                    return false;
                }
            } else if (poll == StdinPollResult::eof) {
                SshChannelEofMessage eof_msg;
                eof_msg.recipient_channel = remote_channel_id;
                (void)send_packet(fd, transport, SshMessageCodec::encode_channel_eof(eof_msg));
                stdin_eof_sent = true;
            } else if (poll == StdinPollResult::error) {
                std::cerr << "failed to read interactive input\n";
                return false;
            }
            return true;
        };

        while (true) {
            std::vector<uint8_t> payload;
            bool should_read_packet = true;
            const auto buffered = transport.try_parse_packet(recv_buf);
            if (buffered.invalid) {
                should_read_packet = true;
            } else if (!buffered.complete) {
                const bool watch_stdin = shell_mode && shell_ready && !stdin_eof_sent;
                const auto ready = wait_client_io(fd, watch_stdin, 20);
                if (ready.stdin_ready && !pump_stdin_once()) {
                    return false;
                }
                should_read_packet = ready.socket_ready;
                if (!should_read_packet) {
                    if (!pump_forward_target_reads()) {
                        std::cerr << "failed to send forwarded data\n";
                        return false;
                    }
                    if (!pump_local_forward_accepts()) {
                        std::cerr << "failed to accept local forward clients\n";
                        return false;
                    }
                    if (!pump_dynamic_forward_accepts() || !pump_dynamic_socks_handshake()) {
                        std::cerr << "failed to process dynamic forwards\n";
                        return false;
                    }
                    continue;
                }
            }
            if (!should_read_packet) {
                continue;
            }
            const auto read_status = read_packet(fd, recv_buf, transport, payload);
            if (read_status == PacketReadStatus::timeout) {
                if (!pump_stdin_once()) {
                    return false;
                }
                if (!pump_forward_target_reads()) {
                    std::cerr << "failed to send forwarded data\n";
                    return false;
                }
                if (!pump_local_forward_accepts()) {
                    std::cerr << "failed to accept local forward clients\n";
                    return false;
                }
                if (!pump_dynamic_forward_accepts() || !pump_dynamic_socks_handshake()) {
                    std::cerr << "failed to process dynamic forwards\n";
                    return false;
                }
                continue;
            }
            if (read_status != PacketReadStatus::ok) {
                cleanup_forward_resources();
                if (has_forward_activity()) {
                    std::cerr << "connection closed while forwarding was active\n";
                    return false;
                }
                if (got_exit_status || session_channel_closed) {
                    return exit_code == 0;
                }
                std::cerr << "connection closed\n";
                return false;
            }
            if (payload.empty()) {
                continue;
            }

            const auto type = static_cast<SshMessageType>(payload[0]);
            debug("packet type " + std::to_string(static_cast<int>(payload[0])));
            if (type == SshMessageType::SSH_MSG_SERVICE_ACCEPT) {
                service_accepted = true;

                if (!identity.public_key_blob.empty()) {
                    if (!send_publickey_probe_request(fd, transport, args.user, identity)) {
                        std::cerr << "failed to send publickey probe auth request\n";
                        return false;
                    }
                    attempted_publickey = true;
                    waiting_pk_ok = true;
                    debug("sent publickey auth probe request");
                } else {
                    if (!send_password_auth_request(fd, transport, args.user, args.password)) {
                        std::cerr << "failed to send password auth request\n";
                        return false;
                    }
                    debug("sent password auth request");
                }
            } else if (type == SshMessageType::SSH_MSG_USERAUTH_SUCCESS) {
                auth_ok = true;
                waiting_pk_ok = false;

                if (!forwarding.send_remote_forward_requests(fd, transport)) {
                    std::cerr << "failed to send tcpip-forward request\n";
                    return false;
                }

                if (!send_session_channel_open(fd, transport, local_channel_id)) {
                    std::cerr << "failed to open channel\n";
                    return false;
                }
                debug("sent channel open");
            } else if (type == SshMessageType::SSH_MSG_USERAUTH_FAILURE) {
                auto fail = SshMessageCodec::decode_userauth_failure(payload.data(), payload.size());
                const std::string methods = fail ? fail->auth_methods_that_can_continue : std::string();

                if (attempted_publickey && args.password.empty()) {
                    std::cerr << "publickey authentication failed";
                    if (!methods.empty()) {
                        std::cerr << " (methods: " << methods << ')';
                    }
                    std::cerr << '\n';
                    return false;
                }

                if (attempted_publickey && !args.password.empty()) {
                    if (!methods.empty() && !list_contains(split_name_list(methods), "password")) {
                        std::cerr << "authentication failed: password method not offered by server";
                        std::cerr << " (methods: " << methods << ')' << '\n';
                        return false;
                    }

                    if (!send_password_auth_request(fd, transport, args.user, args.password)) {
                        std::cerr << "failed to send password auth fallback request\n";
                        return false;
                    }
                    attempted_publickey = false;
                    waiting_pk_ok = false;
                    debug("publickey failed; sent password fallback request");
                    continue;
                }

                std::cerr << "authentication failed";
                if (!methods.empty()) {
                    std::cerr << " (methods: " << methods << ')';
                }
                std::cerr << '\n';
                return false;
            } else if (type == SshMessageType::SSH_MSG_USERAUTH_PK_OK) {
                if (!waiting_pk_ok || identity.public_key_blob.empty()) {
                    std::cerr << "unexpected USERAUTH_PK_OK\n";
                    return false;
                }

                auto pk_ok = SshMessageCodec::decode_userauth_pk_ok(payload.data(), payload.size());
                if (!pk_ok) {
                    std::cerr << "invalid USERAUTH_PK_OK payload\n";
                    return false;
                }
                if (pk_ok->algorithm_name != identity.algorithm || pk_ok->public_key_blob != identity.public_key_blob) {
                    std::cerr << "USERAUTH_PK_OK key mismatch\n";
                    return false;
                }

                auto maybe_signature = make_publickey_signature(
                    transport.session_id(),
                    args.user,
                    identity.algorithm,
                    identity.public_key_blob,
                    identity.private_key_der,
                    crypto);
                if (!maybe_signature) {
                    std::cerr << "failed to sign publickey auth request\n";
                    return false;
                }

                if (!send_signed_publickey_auth_request(fd, transport, args.user, identity, *maybe_signature)) {
                    std::cerr << "failed to send signed publickey auth request\n";
                    return false;
                }

                waiting_pk_ok = false;
                debug("sent signed publickey auth request");
            } else if (type == SshMessageType::SSH_MSG_CHANNEL_OPEN_CONFIRMATION) {
                auto conf = SshMessageCodec::decode_channel_open_confirmation(payload.data(), payload.size());
                if (!conf) {
                    std::cerr << "invalid channel confirmation\n";
                    return false;
                }

                bool handled_forward = false;
                if (!forwarding.handle_open_confirmation(*conf, debug, handled_forward)) {
                    std::cerr << "failed to process forward open confirmation\n";
                    return false;
                }
                if (handled_forward) {
                    continue;
                }

                if (conf->recipient_channel != local_channel_id) {
                    continue;
                }

                remote_channel_id = conf->sender_channel;
                channel_opened = true;

                if (!shell_mode) {
                    if (!send_exec_request(fd, transport, remote_channel_id, args.command)) {
                        std::cerr << "failed to send exec request\n";
                        return false;
                    }
                    exec_sent = true;
                    debug("sent exec request");
                } else if (allocate_pty) {
                    last_terminal_size = query_terminal_size();
                    if (!send_pty_request(fd, transport, remote_channel_id, last_terminal_size)) {
                        std::cerr << "failed to send pty request\n";
                        return false;
                    }
                    pty_sent = true;
                    debug("sent pty request");
                } else {
                    if (!send_shell_request(fd, transport, remote_channel_id)) {
                        std::cerr << "failed to send shell request\n";
                        return false;
                    }
                    shell_sent = true;
                    debug("sent shell request");
                }
            } else if (type == SshMessageType::SSH_MSG_CHANNEL_OPEN_FAILURE) {
                auto open_failure = SshMessageCodec::decode_channel_open_failure(payload.data(), payload.size());
                if (!open_failure) {
                    continue;
                }

                bool handled_forward = false;
                if (!forwarding.handle_open_failure(*open_failure, debug, handled_forward)) {
                    std::cerr << "failed to process forward open failure\n";
                    return false;
                }
                if (handled_forward) {
                    continue;
                }

                if (open_failure->recipient_channel == local_channel_id) {
                    std::cerr << "failed to open session channel: " << open_failure->description << '\n';
                    return false;
                }
            } else if (type == SshMessageType::SSH_MSG_REQUEST_SUCCESS) {
                (void)forwarding.handle_request_success();
            } else if (type == SshMessageType::SSH_MSG_REQUEST_FAILURE) {
                if (!forwarding.handle_request_failure(std::cerr)) {
                    return false;
                }
            } else if (type == SshMessageType::SSH_MSG_CHANNEL_OPEN) {
                auto open = SshMessageCodec::decode_channel_open(payload.data(), payload.size());
                if (!open) {
                    continue;
                }
                bool handled_forward = false;
                if (!forwarding.handle_forwarded_tcpip_open(fd, transport, *open, debug, handled_forward)) {
                    std::cerr << "failed to process remote forward channel open\n";
                    return false;
                }
            } else if (type == SshMessageType::SSH_MSG_CHANNEL_DATA) {
                auto data_msg = SshMessageCodec::decode_channel_data(payload.data(), payload.size());
                if (data_msg && !data_msg->data.empty()) {
                    bool handled_forward = false;
                    if (!forwarding.handle_channel_data(fd, transport, *data_msg, debug, handled_forward)) {
                        std::cerr << "failed to process forwarded channel data\n";
                        return false;
                    }
                    if (!handled_forward) {
                        std::cout.write(reinterpret_cast<const char *>(data_msg->data.data()),
                                        static_cast<std::streamsize>(data_msg->data.size()));
                        std::cout.flush();
                    }
                }
            } else if (type == SshMessageType::SSH_MSG_CHANNEL_EXTENDED_DATA) {
                auto ext_msg = SshMessageCodec::decode_channel_extended_data(payload.data(), payload.size());
                if (ext_msg && !ext_msg->data.empty()) {
                    write_stderr(ext_msg->data);
                }
            } else if (type == SshMessageType::SSH_MSG_CHANNEL_WINDOW_ADJUST) {
                continue;
            } else if (type == SshMessageType::SSH_MSG_CHANNEL_EOF) {
                auto eof_msg = SshMessageCodec::decode_channel_eof(payload.data(), payload.size());
                if (eof_msg) {
                    (void)forwarding.handle_channel_eof(*eof_msg);
                }
            } else if (type == SshMessageType::SSH_MSG_CHANNEL_REQUEST) {
                auto req = SshMessageCodec::decode_channel_request(payload.data(), payload.size());
                if (req && req->request_type == "exit-status") {
                    size_t offset = 0;
                    const uint32_t status = SshMessageCodec::read_uint32(
                        req->request_specific_data.data(), req->request_specific_data.size(), offset);
                    exit_code = status;
                    got_exit_status = true;
                }
            } else if (type == SshMessageType::SSH_MSG_CHANNEL_SUCCESS) {
                auto success = SshMessageCodec::decode_channel_success(payload.data(), payload.size());
                if (!success) {
                    continue;
                }
                if (allocate_pty && pty_sent && !pty_ok) {
                    pty_ok = true;

                    if (!send_shell_request(fd, transport, remote_channel_id)) {
                        std::cerr << "failed to send shell request\n";
                        return false;
                    }
                    shell_sent = true;
                    debug("sent shell request");
                } else if (shell_mode && shell_sent) {
                    if (allocate_pty && !local_terminal_raw.enable()) {
                        std::cerr << "failed to configure local terminal\n";
                        return false;
                    }
                    shell_ready = true;
                    debug("interactive shell ready");
                }
            } else if (type == SshMessageType::SSH_MSG_CHANNEL_FAILURE) {
                if (allocate_pty && pty_sent && !pty_ok) {
                    std::cerr << "pty request failed\n";
                    return false;
                }
                if (shell_mode && shell_sent) {
                    std::cerr << "shell request failed\n";
                    return false;
                }
                if (exec_sent) {
                    std::cerr << "exec request failed\n";
                    return false;
                }
            } else if (type == SshMessageType::SSH_MSG_CHANNEL_CLOSE) {
                auto close_msg = SshMessageCodec::decode_channel_close(payload.data(), payload.size());
                if (close_msg) {
                    bool handled_forward = false;
                    if (!forwarding.handle_channel_close(fd, transport, *close_msg, local_channel_id, handled_forward)) {
                        std::cerr << "failed to process forwarded channel close\n";
                        return false;
                    }
                    if (handled_forward) {
                        continue;
                    }

                    SshChannelCloseMessage back;
                    back.recipient_channel = remote_channel_id;
                    (void)send_packet(fd, transport, SshMessageCodec::encode_channel_close(back));
                }

                session_channel_closed = true;
                if (has_forwarding && has_forward_activity()) {
                    continue;
                }

                forwarding.send_remote_forward_cancel_requests(fd, transport);
                cleanup_forward_resources();
                if (got_exit_status) {
                    return exit_code == 0;
                }
                return true;
            } else if (type == SshMessageType::SSH_MSG_DISCONNECT) {
                auto disc = SshMessageCodec::decode_disconnect(payload.data(), payload.size());
                std::cerr << "disconnected";
                if (disc) {
                    std::cerr << ": " << disc->description;
                }
                std::cerr << '\n';
                return false;
            }

            if (allocate_pty && shell_ready) {
#ifndef _WIN32
                if (g_cli_sigint_count > handled_sigint_count) {
                    handled_sigint_count = g_cli_sigint_count;
                    (void)send_signal_request(fd, transport, remote_channel_id, "INT");
                }

                TerminalSize current_size = query_terminal_size();
                if (current_size.cols != last_terminal_size.cols ||
                    current_size.rows != last_terminal_size.rows ||
                    current_size.pixel_width != last_terminal_size.pixel_width ||
                    current_size.pixel_height != last_terminal_size.pixel_height) {
                    last_terminal_size = current_size;
                    (void)send_window_change_request(fd, transport, remote_channel_id, current_size);
                }
#endif
            }

            if (!pump_forward_target_reads()) {
                std::cerr << "failed to send forwarded data\n";
                return false;
            }
            if (!pump_local_forward_accepts()) {
                std::cerr << "failed to accept local forward clients\n";
                return false;
            }
            if (!pump_dynamic_forward_accepts() || !pump_dynamic_socks_handshake()) {
                std::cerr << "failed to process dynamic forwards\n";
                return false;
            }

            (void)service_accepted;
            (void)auth_ok;
            (void)channel_opened;
        }
    }
}

int main(int argc, char **argv)
{
    yuan::platform::NativePlatformGuard native_guard;
    if (!native_guard.ok()) {
        std::cerr << "native platform init failed\n";
        return 1;
    }

#ifndef _WIN32
    signal(SIGINT, cli_sigint_handler);
    signal(SIGPIPE, SIG_IGN);
#endif

    CliArgs args;
    if (!parse_args(argc, argv, args)) {
        print_usage(argv[0]);
        return 2;
    }
    if (args.help) {
        print_usage(argv[0]);
        return 0;
    }
    if (args.version) {
        std::cout << "release_ssh_cli 1.0\n";
        return 0;
    }

    SocketGuard sock;
    if (!connect_tcp(args.host, static_cast<uint16_t>(args.port), sock)) {
        std::cerr << "connect failed: " << args.host << ':' << args.port << '\n';
        return 1;
    }
    (void)set_recv_timeout(sock.fd, args.timeout_ms);

    const bool ok = args.probe ? run_version_probe(sock.fd, args) : run_exec_phase1(sock.fd, args);
    return ok ? 0 : 1;
}
