#include "ssh_server.h"
#include "auth/ssh_auth_password.h"
#include "auth/ssh_auth_publickey.h"
#include "auth/ssh_auth_keyboard_interactive.h"
#include "connection/ssh_port_forwarding.h"
#include "crypto/ssh_crypto_openssl.h"
#include "base/owner_ptr.h"

#if YUAN_ENABLE_SSH_SFTP
#include "sftp/ssh_sftp_subsystem.h"
#endif
#include "protocol/ssh_message_codec.h"
#include "transport/ssh_version_exchange.h"
#include "transport/ssh_packet_codec.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <iostream>
#include <string_view>

namespace yuan::net::ssh
{
    namespace
    {
        bool contains_token_ci(const std::string & text, std::string_view token)
        {
            if (token.empty() || text.size() < token.size()) {
                return false;
            }

            for (size_t i = 0; i + token.size() <= text.size(); ++i) {
                size_t j = 0;
                while (j < token.size()) {
                    const auto lhs = static_cast<unsigned char>(text[i + j]);
                    const auto rhs = static_cast<unsigned char>(token[j]);
                    if (std::tolower(lhs) != std::tolower(rhs)) {
                        break;
                    }
                    ++j;
                }
                if (j == token.size()) {
                    return true;
                }
            }
            return false;
        }

        SshHostKeyType infer_host_key_type(const std::string & path)
        {
            if (contains_token_ci(path, "ed25519")) {
                return SshHostKeyType::ED25519;
            }
            if (contains_token_ci(path, "ecdsa") && contains_token_ci(path, "521")) {
                return SshHostKeyType::ECDSA_P521;
            }
            if (contains_token_ci(path, "ecdsa") && contains_token_ci(path, "384")) {
                return SshHostKeyType::ECDSA_P384;
            }
            if (contains_token_ci(path, "ecdsa")) {
                return SshHostKeyType::ECDSA_P256;
            }
            return SshHostKeyType::RSA;
        }
    }

    SshServer::SshServer()
        : config_(), crypto_(std::make_unique<SshCryptoOpenSSL>())
    {
    }

    SshServer::SshServer(const SshServerConfig & config)
        : config_(config), crypto_(std::make_unique<SshCryptoOpenSSL>())
    {
    }

    SshServer::~SshServer()
    {
        stop();
    }

    bool SshServer::init(int port)
    {
        config_.port = static_cast<uint16_t>(port);
        auto runtime = std::make_unique<NetworkRuntime>();
        owned_runtime_ = std::move(runtime);
        return init(port, *owned_runtime_);
    }

    bool SshServer::init(int port, NetworkRuntime & runtime)
    {
        config_.port = static_cast<uint16_t>(port);

        if (config_.enable_openssh_compat_profile) {
            config_.enable_builtin_terminal_handler = true;
            // Prefer conservative, widely interoperable algorithms in compat mode.
            config_.cipher_algorithms = {
                "aes256-ctr",
                "aes192-ctr",
                "aes128-ctr"
            };
            config_.mac_algorithms = {
                "hmac-sha2-256",
                "hmac-sha2-512",
                "hmac-sha1"
            };
            config_.compression_algorithms = {
                "none"
            };
        }

        init_default_algorithms();

        bool loaded_host_key = false;
        for (const auto &path : config_.host_key_paths) {
            loaded_host_key = host_key_provider_.load_key(path, infer_host_key_type(path)) || loaded_host_key;
        }

        if (!loaded_host_key && !config_.host_key_paths.empty()) {
            return false;
        }

        if (config_.enable_sftp) {
#if YUAN_ENABLE_SSH_SFTP
            if (!file_system_) {
#ifdef _WIN32
                const std::string sftp_root = config_.sftp_root_dir.empty()
                                                  ? std::filesystem::current_path().string()
                                                  : config_.sftp_root_dir;
#else
                const std::string sftp_root = config_.sftp_root_dir.empty() ? "/" : config_.sftp_root_dir;
#endif
                file_system_ = std::make_unique<SshLocalFileSystem>(
                    sftp_root);
            }
            auto *fs = yuan::base::owner_ptr(file_system_);
            register_subsystem("sftp", [fs]()->std::unique_ptr<SshChannelHandler> {
                return std::make_unique<SshSftpSubsystem>(fs);
            });
#endif
        }

        if (!listener_.bind(port, runtime)) {
            return false;
        }

        return true;
    }

    void SshServer::serve()
    {
        running_.store(true, std::memory_order_relaxed);

        listener_.set_connection_handler(
            [this](AsyncConnectionContext ctx)->coroutine::Task<void> {
                co_await handle_connection(std::move(ctx));
            });

        auto task = listener_.run_async();
        task.resume();
        task.detach();

        if (owned_runtime_) {
            owned_runtime_->run();
        }
    }

    void SshServer::stop()
    {
        bool expected = true;
        if (!running_.compare_exchange_strong(expected, false)) {
            return;
        }

        listener_.close();
        session_mgr_.close_all();

        if (owned_runtime_) {
            owned_runtime_->stop();
        }
    }

    void SshServer::register_subsystem(const std::string & name, SshConnectionManager::SubsystemFactory factory)
    {
        subsystem_factories_[name] = std::move(factory);
    }

    coroutine::Task<void> SshServer::handle_connection(AsyncConnectionContext ctx)
    {
        std::cerr << "[ssh] incoming connection, active_sessions=" << session_mgr_.session_count() << '\n';
        auto *effective_handler = handler_ ? handler_ : &SshHandler::default_handler();
        if (!handler_ && config_.enable_builtin_terminal_handler) {
            effective_handler = &builtin_terminal_handler_;
        }

        if (session_mgr_.session_limit_reached(config_.max_sessions)) {
            std::cerr << "[ssh] session limit reached: " << session_mgr_.session_count()
                      << "/" << config_.max_sessions << '\n';
            ctx.close();
            co_return;
        }

        auto *session = session_mgr_.create_session(this);
        if (!session) {
            std::cerr << "[ssh] create_session failed\n";
            ctx.close();
            co_return;
        }
        std::cerr << "[ssh] session created id=" << session->session_id()
                  << " active_sessions=" << session_mgr_.session_count() << '\n';

        session->transport() = SshTransport(&algo_registry_, yuan::base::owner_ptr(crypto_), true);

        session->set_client_connection(ctx.connection());
        session->set_runtime(ctx.runtime_view());

        for (const auto & [
                              name,
                              factory
                          ] : subsystem_factories_) {
            session->connection_manager().register_subsystem(name, factory);
        }

        init_auth_methods(session);

        ByteBuffer recv_buf;

        // 1. Send server version
        auto server_version = session->transport().build_version_string(config_.software_version);
        session->transport().set_server_version(server_version.substr(0, server_version.size() - 2));
        {
            ByteBuffer version_buf{std::string_view(server_version)};
            auto w = co_await ctx.write_async(version_buf, config_.idle_timeout_ms);
            if (w.status != coroutine::IoStatus::success) {
                std::cerr << "[ssh] write server version failed, session=" << session->session_id() << '\n';
                ctx.close();
                session_mgr_.remove_session(session->session_id());
                co_return;
            }
            auto f = co_await ctx.flush_async(config_.idle_timeout_ms);
            if (f.status != coroutine::IoStatus::success) {
                std::cerr << "[ssh] flush server version failed, session=" << session->session_id() << '\n';
                ctx.close();
                session_mgr_.remove_session(session->session_id());
                co_return;
            }
        }

        // 2. Read client version
        {
            auto read_result = co_await ctx.read_async(config_.idle_timeout_ms);
            if (read_result.status != coroutine::IoStatus::success) {
                std::cerr << "[ssh] read client version failed, session=" << session->session_id() << '\n';
                ctx.close();
                session_mgr_.remove_session(session->session_id());
                co_return;
            }

            auto &data = read_result.data;
            auto version_end = SshVersionExchange::find_version_line_end(
                reinterpret_cast<const uint8_t *>(data.read_ptr()), data.readable_bytes());
            if (!version_end) {
                std::cerr << "[ssh] client version line incomplete, session=" << session->session_id() << '\n';
                ctx.close();
                session_mgr_.remove_session(session->session_id());
                co_return;
            }

            std::string version_line(data.read_ptr(), *version_end - 2);
            auto version_info = SshVersionExchange::parse_version_line(version_line);
            if (!version_info || !SshVersionExchange::is_valid_protocol_version(version_info->protocol_version)) {
                std::cerr << "[ssh] invalid client version, session=" << session->session_id() << '\n';
                ctx.close();
                session_mgr_.remove_session(session->session_id());
                co_return;
            }

            session->transport().set_client_version(version_info->raw_line);
            data.consume(*version_end);
            if (data.readable_bytes() > 0) {
                recv_buf.append(data);
            }
            session->set_state(SshSession::State::version_exchanged);
        }

        // 3. Send KEXINIT
        {
            auto kex_init_buf = session->transport().build_kex_init(config_);
            auto packet = session->transport().encode_packet(
                reinterpret_cast<const uint8_t *>(kex_init_buf.read_ptr()), kex_init_buf.readable_bytes());
            session->transport().increment_send_seq();
            ctx.write_and_flush(packet);
        }

        // 4. Main packet processing loop

        auto flush_outgoing = [&]()->coroutine::Task<void>
        {
            auto buffers = session->drain_outgoing();
            for (auto &buf : buffers) {
                auto packet = session->transport().encode_packet(
                    reinterpret_cast<const uint8_t *>(buf.read_ptr()), buf.readable_bytes());
                session->transport().increment_send_seq();
                ctx.write_and_flush(packet);
            }
            co_return;
        };

        auto send_packet = [&](const ByteBuffer & buf)->coroutine::Task<void>
        {
            auto packet = session->transport().encode_packet(
                reinterpret_cast<const uint8_t *>(buf.read_ptr()), buf.readable_bytes());
            session->transport().increment_send_seq();
            ctx.write_and_flush(packet);
            co_return;
        };

        while (session->state() != SshSession::State::disconnected && running_.load(std::memory_order_relaxed)) {
            if (recv_buf.readable_bytes() == 0) {
                uint32_t read_timeout_ms = config_.idle_timeout_ms;
                if (session->has_any_pty_processes()) {
                    if (read_timeout_ms == 0 || read_timeout_ms > 2) {
                        read_timeout_ms = 2;
                    }
                }

                auto read_result = co_await ctx.read_async(read_timeout_ms);
                if (read_result.status == coroutine::IoStatus::success) {
                    recv_buf.append(read_result.data);
                } else if (read_result.status == coroutine::IoStatus::timed_out) {
                    if (session->pump_all_pty_once(effective_handler)) {
                        co_await flush_outgoing();
                    }
                    continue;
                } else {
                    break;
                }
            }

            session->connection_manager().poll_async_tasks();
            auto forwarded_opens = session->drain_pending_forwarded_tcpip_open();
            for (auto &open_packet : forwarded_opens) {
                co_await send_packet(open_packet);
            }

            while (recv_buf.readable_bytes() > 0) {
                auto parse = session->transport().try_parse_packet(recv_buf);
                if (parse.invalid) {
                    auto disc = session->build_disconnect(
                        SshDisconnectReason::SSH_DISCONNECT_PROTOCOL_ERROR, "Invalid SSH packet");
                    co_await send_packet(disc);
                    goto session_end;
                }
                if (!parse.complete) {
                    uint32_t read_timeout_ms = config_.idle_timeout_ms;
                    if (session->has_any_pty_processes()) {
                        if (read_timeout_ms == 0 || read_timeout_ms > 2) {
                            read_timeout_ms = 2;
                        }
                    }

                    auto read_result = co_await ctx.read_async(read_timeout_ms);
                    if (read_result.status == coroutine::IoStatus::success) {
                        recv_buf.append(read_result.data);
                    } else if (read_result.status == coroutine::IoStatus::timed_out) {
                        if (session->pump_all_pty_once(effective_handler)) {
                            co_await flush_outgoing();
                        }
                    } else {
                        goto session_end;
                    }
                    continue;
                }

                auto payload = session->transport().decode_packet(
                    reinterpret_cast<const uint8_t *>(recv_buf.read_ptr()), parse.total_bytes);
                session->transport().increment_recv_seq();

                if (!payload) {
                    auto disc = session->build_disconnect(
                        SshDisconnectReason::SSH_DISCONNECT_MAC_ERROR, "Decryption failed");
                    co_await send_packet(disc);
                    goto session_end;
                }

                recv_buf.consume(parse.total_bytes);

                session->connection_manager().poll_async_tasks();
                auto forwarded_opens = session->drain_pending_forwarded_tcpip_open();
                for (auto &open_packet : forwarded_opens) {
                    co_await send_packet(open_packet);
                }

                if (payload->empty()) {
                    continue;
                }

                auto msg_type = static_cast<SshMessageType>((*payload)[0]);

                // Handle DISCONNECT from any state
                if (msg_type == SshMessageType::SSH_MSG_DISCONNECT) {
                    session->set_state(SshSession::State::disconnected);
                    goto session_end;
                }

                // Handle KEXINIT from any state
                if (msg_type == SshMessageType::SSH_MSG_KEXINIT) {
                    if (session->state() != SshSession::State::version_exchanged) {
                        session->set_pre_rekey_state(session->state());
                        session->transport().reset_for_rekey();
                    }

                    session->transport().set_peer_kex_init_raw(*payload);

                    auto kex_init = SshMessageCodec::decode_kex_init(payload->data(), payload->size());
                    if (kex_init) {
                        auto negotiated = session->transport().process_kex_init(*kex_init, config_);
                        if (!negotiated) {
                            goto session_end;
                        }

                        const auto &kex_list = kex_init->kex_algorithms;
                        const bool peer_supports_strict =
                            (kex_list.find("kex-strict-c-v00@openssh.com") != std::string::npos);
                        const bool server_supports_strict = std::find(
                            config_.kex_algorithms.begin(),
                            config_.kex_algorithms.end(),
                            "kex-strict-s-v00@openssh.com") != config_.kex_algorithms.end();
                        session->set_kex_strict_mode(peer_supports_strict && server_supports_strict);

                        auto *host_key_algorithm = host_key_provider_.find_algorithm(negotiated->host_key_name);
                        if (!host_key_algorithm) {
                            goto session_end;
                        }
                        session->transport().set_host_key_algorithm(host_key_algorithm);

                        // For rekey, send our KEXINIT in response
                        if (session->pre_rekey_state() != SshSession::State::connected) {
                            auto kex_init_buf = session->transport().build_kex_init(config_);
                            co_await send_packet(kex_init_buf);
                        }

                        session->set_state(SshSession::State::kex_init);
                    }
                    continue;
                }

                // Handle DH/ECDH init when in kex_init
                if (session->state() == SshSession::State::kex_init &&
                    (msg_type == SshMessageType::SSH_MSG_KEX_ECDH_INIT ||
                     msg_type == SshMessageType::SSH_MSG_KEXDH_INIT ||
                     static_cast<uint8_t>((*payload)[0]) == 30)) {
                    // libstdc++ treats enum values with duplicated underlying value
                    // as the first declared enumerator during comparisons.
                    // Here we must branch by raw packet type byte to correctly handle
                    // ECDH (30) vs DH (30 legacy alias) init payload formats.
                    const uint8_t msg_type_raw = (*payload)[0];
                    if (session->transport().consume_pending_kex_guess()) {
                        continue;
                    }

                    std::optional<std::vector<uint8_t>> client_public;
                    if (msg_type_raw == 30) {
                        auto ecdh_init = SshMessageCodec::decode_kex_ecdh_init(payload->data(), payload->size());
                        if (ecdh_init) {
                            client_public = std::move(ecdh_init->client_public_key);
                        }
                    } else {
                        size_t offset = 1;
                        client_public = SshMessageCodec::read_mpint(payload->data(), payload->size(), offset);
                    }

                    if (client_public) {
                        auto reply = session->transport().process_kex_init_message(
                            *client_public,
                            session->transport().client_version(),
                            session->transport().server_version());
                        if (reply) {
                            auto reply_buf = SshMessageCodec::encode_kex_ecdh_reply(*reply);
                            co_await send_packet(reply_buf);

                            auto newkeys_buf = SshMessageCodec::encode_newkeys();
                            co_await send_packet(newkeys_buf);

                            // Do NOT call process_newkeys() here.
                            // New keys are activated when client's NEWKEYS is received,
                            // ensuring old keys are used for receiving until then.
                            session->set_state(SshSession::State::newkeys);
                        } else {
                            goto session_end;
                        }
                    } else {
                        goto session_end;
                    }
                    continue;
                }

                // Handle NEWKEYS from client — activate new keys
                if (msg_type == SshMessageType::SSH_MSG_NEWKEYS) {
                    if (session->kex_strict_mode()) {
                        session->transport().reset_packet_sequences();
                    }
                    session->transport().process_newkeys();

                    if (session->pre_rekey_state() != SshSession::State::connected &&
                        session->pre_rekey_state() != SshSession::State::version_exchanged) {
                        // Rekey completed — restore pre-rekey state
                        session->set_state(session->pre_rekey_state());
                        session->set_pre_rekey_state(SshSession::State::connected);
                    } else {
                        // Initial KEX completed
                        session->set_state(SshSession::State::newkeys);
                    }
                    continue;
                }

                // Handle IGNORE and DEBUG silently
                if (msg_type == SshMessageType::SSH_MSG_IGNORE ||
                    msg_type == SshMessageType::SSH_MSG_DEBUG) {
                    continue;
                }

                // Dispatch non-KEX messages to session
                session->dispatch(msg_type, *payload, effective_handler);

                // Send responses based on session state changes
                if (session->state() == SshSession::State::auth_start) {
                    auto accept = session->build_service_accept(SSH_SERVICE_USERAUTH);
                    co_await send_packet(accept);
                    if (!config_.banner.empty()) {
                        auto banner = session->build_userauth_banner(config_.banner);
                        co_await send_packet(banner);
                    }
                } else if (session->state() == SshSession::State::auth_success) {
                    auto success = session->build_userauth_success();
                    co_await send_packet(success);

                    effective_handler->on_session_opened(session);
                } else if (session->state() == SshSession::State::auth_need_more) {
                    auto pending = session->authenticator().pending_auth_response();
                    if (pending == SshAuthenticator::PendingAuthResponse::pk_ok) {
                        auto pk_ok = session->build_userauth_pk_ok(
                            session->authenticator().pending_pk_algo(),
                            session->authenticator().pending_pk_key_blob());
                        co_await send_packet(pk_ok);
                    } else if (pending == SshAuthenticator::PendingAuthResponse::info_request) {
                        auto *method = session->authenticator().active_method();
                        SshUserauthInfoRequestMessage info_req;
                        if (method) {
                            info_req = method->build_challenge(session, session->authenticator().username());
                        }
                        auto info_buf = session->build_userauth_info_request(info_req);
                        co_await send_packet(info_buf);
                    }
                } else if (session->state() == SshSession::State::authenticating) {
                    if (config_.auth_failure_delay_ms > 0) {
                        co_await ctx.runtime_view().sleep_for(config_.auth_failure_delay_ms);
                    }
                    auto failure = session->build_userauth_failure(false);
                    co_await send_packet(failure);
                }

                co_await flush_outgoing();
            }

            if (session->has_any_pty_processes()) {
                if (session->pump_all_pty_once(effective_handler)) {
                    co_await flush_outgoing();
                }
            }
        }

    session_end:
        ctx.close();
        effective_handler->on_session_closed(session);
        session->set_state(SshSession::State::disconnected);
        session_mgr_.remove_session(session->session_id());
        std::cerr << "[ssh] session removed id=" << session->session_id()
                  << " active_sessions=" << session_mgr_.session_count() << '\n';
    }

    void SshServer::init_default_algorithms()
    {
        // Algorithm registrations are done in the algorithm registry implementation
        // The registry is populated with factory functions for each algorithm
        algo_registry_.register_defaults();
    }

    void SshServer::init_auth_methods(SshSession * session)
    {
        for (const auto &method_name : config_.auth_methods) {
            if (method_name == "password") {
                session->authenticator().register_method(std::make_unique<SshAuthPassword>());
            } else if (method_name == "publickey") {
                session->authenticator().register_method(std::make_unique<SshAuthPublickey>(yuan::base::owner_ptr(crypto_)));
            } else if (method_name == "keyboard-interactive") {
                session->authenticator().register_method(std::make_unique<SshAuthKeyboardInteractive>());
            }
        }
    }
}
