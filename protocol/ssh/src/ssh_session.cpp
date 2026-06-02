#include "ssh_session.h"
#include "ssh_handler.h"
#include "ssh_config.h"
#include "protocol/ssh_message_codec.h"
#include "base/owner_ptr.h"

#include <algorithm>

namespace yuan::net::ssh
{
    SshSession::SshSession(uint64_t session_id, SshServer * server)
        : session_id_(session_id), server_(server),
          transport_(nullptr, nullptr, true),
          authenticator_(),
          conn_mgr_(this),
          terminal_bridge_(std::make_unique<SshTerminalBridge>(this, &conn_mgr_))
    {
    }

    SshSession::~SshSession()
    {
        shutdown_all_pty_processes();
        conn_mgr_.close_all_channels();
    }

    void SshSession::enqueue_outgoing(ByteBuffer buf)
    {
        if (buf.readable_bytes() == 0)
            return;
        std::lock_guard<std::mutex> lock(outgoing_mutex_);
        outgoing_.push_back(std::move(buf));
    }

    std::vector<ByteBuffer> SshSession::drain_outgoing()
    {
        std::vector<ByteBuffer> result;
        std::lock_guard<std::mutex> lock(outgoing_mutex_);
        result.reserve(outgoing_.size());
        for (auto &buf : outgoing_) {
            result.push_back(std::move(buf));
        }
        outgoing_.clear();
        return result;
    }

    void SshSession::flush_channel_pending_data()
    {
        auto buffers = conn_mgr_.drain_channel_pending_data();
        for (auto &buf : buffers) {
            enqueue_outgoing(std::move(buf));
        }
    }

    void SshSession::enqueue_pending_forwarded_tcpip_open(ByteBuffer buf)
    {
        if (buf.readable_bytes() == 0) {
            return;
        }
        std::lock_guard<std::mutex> lock(pending_forwarded_open_mutex_);
        pending_forwarded_opens_.push_back(std::move(buf));
    }

    std::vector<ByteBuffer> SshSession::drain_pending_forwarded_tcpip_open()
    {
        std::vector<ByteBuffer> result;
        std::lock_guard<std::mutex> lock(pending_forwarded_open_mutex_);
        result.reserve(pending_forwarded_opens_.size());
        for (auto &buf : pending_forwarded_opens_) {
            result.push_back(std::move(buf));
        }
        pending_forwarded_opens_.clear();
        return result;
    }

    void SshSession::register_pty_process(uint32_t channel_remote_id, std::unique_ptr<SshPtyProcess> process)
    {
        if (terminal_bridge_) {
            terminal_bridge_->register_pty_process(channel_remote_id, std::move(process));
        }
    }

    bool SshSession::has_pty_process(uint32_t channel_remote_id) const
    {
        return terminal_bridge_ && terminal_bridge_->has_pty_process(channel_remote_id);
    }

    bool SshSession::has_any_pty_processes() const
    {
        return terminal_bridge_ && terminal_bridge_->has_any_pty_processes();
    }

    int SshSession::first_pty_master_fd() const
    {
        return terminal_bridge_ ? terminal_bridge_->first_pty_master_fd() : -1;
    }

    bool SshSession::pump_pty_once(uint32_t channel_remote_id, SshHandler * handler)
    {
        return terminal_bridge_ && terminal_bridge_->pump_pty_once(channel_remote_id, handler);
    }

    bool SshSession::pump_all_pty_once(SshHandler * handler)
    {
        return terminal_bridge_ && terminal_bridge_->pump_all_pty_once(handler);
    }

    void SshSession::shutdown_pty_for_channel(uint32_t channel_remote_id)
    {
        if (terminal_bridge_) {
            terminal_bridge_->shutdown_pty_for_channel(channel_remote_id);
        }
    }

    void SshSession::shutdown_all_pty_processes()
    {
        if (terminal_bridge_) {
            terminal_bridge_->shutdown_all_pty_processes();
        }
    }

    void SshSession::dispatch(SshMessageType msg_type, const std::vector<uint8_t> & payload, SshHandler * handler)
    {
        switch (state_) {
        case State::newkeys:
            if (msg_type == SshMessageType::SSH_MSG_SERVICE_REQUEST) {
                handle_service_request(payload, handler);
            } else if (msg_type == SshMessageType::SSH_MSG_DISCONNECT) {
                set_state(State::disconnected);
            }
            break;

        case State::auth_start:
        case State::authenticating:
        case State::auth_need_more:
            if (msg_type == SshMessageType::SSH_MSG_SERVICE_REQUEST) {
                handle_service_request(payload, handler);
            } else if (msg_type == SshMessageType::SSH_MSG_USERAUTH_REQUEST) {
                handle_userauth_request(payload, handler);
            } else if (msg_type == SshMessageType::SSH_MSG_USERAUTH_INFO_RESPONSE) {
                handle_userauth_info_response(payload, handler);
            } else if (msg_type == SshMessageType::SSH_MSG_DISCONNECT) {
                set_state(State::disconnected);
            }
            break;

        case State::auth_success:
        case State::active:
            if (msg_type == SshMessageType::SSH_MSG_SERVICE_REQUEST) {
                handle_service_request(payload, handler);
            } else if (msg_type == SshMessageType::SSH_MSG_CHANNEL_OPEN ||
                       msg_type == SshMessageType::SSH_MSG_CHANNEL_DATA ||
                       msg_type == SshMessageType::SSH_MSG_CHANNEL_EOF ||
                       msg_type == SshMessageType::SSH_MSG_CHANNEL_CLOSE ||
                       msg_type == SshMessageType::SSH_MSG_CHANNEL_WINDOW_ADJUST ||
                       msg_type == SshMessageType::SSH_MSG_CHANNEL_REQUEST ||
                       msg_type == SshMessageType::SSH_MSG_CHANNEL_EXTENDED_DATA ||
                       msg_type == SshMessageType::SSH_MSG_GLOBAL_REQUEST) {
                set_state(State::active);
                handle_connection_message(msg_type, payload, handler);
            } else if (msg_type == SshMessageType::SSH_MSG_DISCONNECT) {
                set_state(State::disconnected);
            }
            break;

        default:
            if (msg_type == SshMessageType::SSH_MSG_DISCONNECT) {
                set_state(State::disconnected);
            }
            break;
        }
    }

    void SshSession::handle_service_request(const std::vector<uint8_t> & payload, SshHandler * handler)
    {
        auto msg = SshMessageCodec::decode_service_request(payload.data(), payload.size());
        if (!msg) {
            return;
        }

        if (msg->service_name == SSH_SERVICE_USERAUTH) {
            if (authenticator_.process_service_request(msg->service_name)) {
                set_state(State::auth_start);
            }
        } else if (msg->service_name == SSH_SERVICE_CONNECTION) {
            if (authenticated()) {
                set_state(State::auth_success);
            }
        }
    }

    void SshSession::handle_userauth_request(const std::vector<uint8_t> & payload, SshHandler * handler)
    {
        auto msg = SshMessageCodec::decode_userauth_request(payload.data(), payload.size());
        if (!msg) {
            return;
        }

        SshAuthResult result = authenticator_.process_userauth_request(this, handler, *msg);

        if (result == SshAuthResult::SUCCESS) {
            set_state(State::auth_success);
        } else if (result == SshAuthResult::NEED_MORE) {
            set_state(State::auth_need_more);
        } else {
            set_state(State::authenticating);
            if (authenticator_.max_attempts_exceeded()) {
                set_state(State::disconnected);
            }
        }
    }

    void SshSession::handle_userauth_info_response(const std::vector<uint8_t> & payload, SshHandler * handler)
    {
        auto msg = SshMessageCodec::decode_userauth_info_response(payload.data(), payload.size());
        if (!msg) {
            return;
        }

        SshAuthResult result = authenticator_.process_info_response(this, handler, *msg);

        if (result == SshAuthResult::SUCCESS) {
            set_state(State::auth_success);
        } else if (result == SshAuthResult::NEED_MORE) {
            set_state(State::auth_need_more);
        } else {
            set_state(State::authenticating);
            if (authenticator_.max_attempts_exceeded()) {
                set_state(State::disconnected);
            }
        }
    }

    void SshSession::handle_connection_message(SshMessageType msg_type, const std::vector<uint8_t> & payload,
                                               SshHandler * handler)
    {
        ByteBuffer resp;

        switch (msg_type) {
        case SshMessageType::SSH_MSG_CHANNEL_OPEN: {
            auto msg = SshMessageCodec::decode_channel_open(payload.data(), payload.size());
            if (msg) {
                resp = conn_mgr_.handle_channel_open(*msg, handler);
            }
            break;
        }
        case SshMessageType::SSH_MSG_CHANNEL_OPEN_CONFIRMATION: {
            auto msg = SshMessageCodec::decode_channel_open_confirmation(payload.data(), payload.size());
            if (msg) {
                conn_mgr_.handle_channel_open_confirmation(*msg);
            }
            break;
        }
        case SshMessageType::SSH_MSG_CHANNEL_OPEN_FAILURE: {
            auto msg = SshMessageCodec::decode_channel_open_failure(payload.data(), payload.size());
            if (msg) {
                conn_mgr_.handle_channel_open_failure(*msg);
            }
            break;
        }
        case SshMessageType::SSH_MSG_CHANNEL_DATA: {
            auto msg = SshMessageCodec::decode_channel_data(payload.data(), payload.size());
            if (msg) {
                resp = conn_mgr_.handle_channel_data(*msg, handler);
                if (terminal_bridge_) {
                    terminal_bridge_->handle_channel_data(*msg, handler);
                }
            }
            break;
        }
        case SshMessageType::SSH_MSG_CHANNEL_EXTENDED_DATA: {
            auto msg = SshMessageCodec::decode_channel_extended_data(payload.data(), payload.size());
            if (msg) {
                resp = conn_mgr_.handle_channel_extended_data(*msg);
            }
            break;
        }
        case SshMessageType::SSH_MSG_CHANNEL_WINDOW_ADJUST: {
            auto msg = SshMessageCodec::decode_channel_window_adjust(payload.data(), payload.size());
            if (msg) {
                conn_mgr_.handle_channel_window_adjust(*msg);
            }
            break;
        }
        case SshMessageType::SSH_MSG_CHANNEL_EOF: {
            auto msg = SshMessageCodec::decode_channel_eof(payload.data(), payload.size());
            if (msg) {
                resp = conn_mgr_.handle_channel_eof(*msg);
            }
            break;
        }
        case SshMessageType::SSH_MSG_CHANNEL_CLOSE: {
            auto msg = SshMessageCodec::decode_channel_close(payload.data(), payload.size());
            if (msg) {
                auto *channel = conn_mgr_.find_channel_by_remote(msg->recipient_channel);
                if (channel && channel->command_started() && channel->mark_termination_notified()) {
                    auto *effective_handler = handler ? handler : &SshHandler::default_handler();
                    auto exit_info = effective_handler->on_command_exit(this, channel);
                    if (exit_info.use_signal) {
                        enqueue_outgoing(conn_mgr_.build_channel_exit_signal(channel->remote_id(),
                                                                             exit_info.signal_name,
                                                                             exit_info.core_dumped,
                                                                             exit_info.error_message,
                                                                             exit_info.language_tag));
                    } else {
                        enqueue_outgoing(conn_mgr_.build_channel_exit_status(channel->remote_id(),
                                                                             exit_info.exit_status));
                    }
                }
                shutdown_pty_for_channel(msg->recipient_channel);
                resp = conn_mgr_.handle_channel_close(*msg, handler);
            }
            break;
        }
        case SshMessageType::SSH_MSG_CHANNEL_REQUEST: {
            auto msg = SshMessageCodec::decode_channel_request(payload.data(), payload.size());
            if (msg) {
                resp = conn_mgr_.handle_channel_request(*msg, handler);
                if (terminal_bridge_) {
                    terminal_bridge_->handle_channel_request(*msg, resp, handler);
                }
            }
            break;
        }
        case SshMessageType::SSH_MSG_GLOBAL_REQUEST: {
            auto msg = SshMessageCodec::decode_global_request(payload.data(), payload.size());
            if (msg) {
                resp = conn_mgr_.handle_global_request(*msg, handler);
            }
            break;
        }
        case SshMessageType::SSH_MSG_DISCONNECT: {
            set_state(State::disconnected);
            break;
        }
        default:
            break;
        }

        if (resp.readable_bytes() > 0) {
            enqueue_outgoing(std::move(resp));
        }

        flush_channel_pending_data();
    }

    ByteBuffer SshSession::build_service_accept(const std::string & service_name) const
    {
        SshServiceAcceptMessage msg;
        msg.service_name = service_name;
        return SshMessageCodec::encode_service_accept(msg);
    }

    ByteBuffer SshSession::build_userauth_banner(const std::string & message) const
    {
        SshUserauthBannerMessage msg;
        msg.message = message;
        msg.language = "en";
        return SshMessageCodec::encode_userauth_banner(msg);
    }

    ByteBuffer SshSession::build_userauth_success() const
    {
        return SshMessageCodec::encode_userauth_success();
    }

    ByteBuffer SshSession::build_userauth_failure(bool partial_success) const
    {
        SshUserauthFailureMessage msg;
        msg.auth_methods_that_can_continue = authenticator_.allowed_methods_string();
        msg.partial_success = partial_success;
        return SshMessageCodec::encode_userauth_failure(msg);
    }

    ByteBuffer SshSession::build_userauth_pk_ok(const std::string & algo, const std::vector<uint8_t> & key_blob) const
    {
        SshUserauthPkOkMessage msg;
        msg.algorithm_name = algo;
        msg.public_key_blob = key_blob;
        return SshMessageCodec::encode_userauth_pk_ok(msg);
    }

    ByteBuffer SshSession::build_userauth_info_request(const SshUserauthInfoRequestMessage & msg) const
    {
        return SshMessageCodec::encode_userauth_info_request(msg);
    }

    ByteBuffer SshSession::build_disconnect(SshDisconnectReason reason, const std::string & description) const
    {
        SshDisconnectMessage msg;
        msg.reason_code = static_cast<uint32_t>(reason);
        msg.description = description;
        msg.language = "en";
        return SshMessageCodec::encode_disconnect(msg);
    }

    // ---- SshSessionManager ----

    SshSession *SshSessionManager::create_session(SshServer * server)
    {
        auto id = next_session_id_.fetch_add(1, std::memory_order_relaxed);
        auto session = std::make_unique<SshSession>(id, server);
        auto *ptr = yuan::base::owner_ptr(session);
        std::lock_guard<std::mutex> lock(mutex_);
        sessions_.emplace(id, std::move(session));
        return ptr;
    }

    SshSession *SshSessionManager::find_session(uint64_t session_id)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = sessions_.find(session_id);
        return it != sessions_.end() ? yuan::base::owner_ptr(it->second) : nullptr;
    }

    void SshSessionManager::remove_session(uint64_t session_id)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        sessions_.erase(session_id);
    }

    void SshSessionManager::close_all()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto & [
                        id,
                        session
                    ] : sessions_) {
            session->set_state(SshSession::State::disconnected);
            session->connection_manager().close_all_channels();
        }
        sessions_.clear();
    }

    uint32_t SshSessionManager::session_count() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return static_cast<uint32_t>(sessions_.size());
    }

    bool SshSessionManager::session_limit_reached(uint32_t max_sessions) const
    {
        return session_count() >= max_sessions;
    }
}
