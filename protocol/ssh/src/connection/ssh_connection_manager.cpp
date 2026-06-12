#include "connection/ssh_connection_manager.h"
#include "connection/ssh_global_request.h"
#include "connection/ssh_port_forwarding.h"
#include "connection/ssh_direct_tcpip_handler.h"
#include "protocol/ssh_message_codec.h"
#include "ssh_handler.h"
#include "ssh_config.h"
#include "ssh_session.h"
#include "ssh_server.h"
#include "base/owner_ptr.h"

#include <algorithm>

namespace yuan::net::ssh
{
    namespace
    {
        struct DirectTcpipOpenData
        {
            std::string target_host;
            uint16_t target_port = 0;
            std::string originator_host;
            uint16_t originator_port = 0;
        };

        std::optional<DirectTcpipOpenData> decode_direct_tcpip_open_data(const SshChannelOpenMessage &msg)
        {
            size_t offset = 0;
            auto target_host = SshMessageCodec::read_string(msg.type_specific_data.data(),
                                                            msg.type_specific_data.size(), offset);
            if (!target_host) {
                return std::nullopt;
            }

            if (offset + 4 > msg.type_specific_data.size()) {
                return std::nullopt;
            }
            uint32_t target_port = SshMessageCodec::read_uint32(msg.type_specific_data.data(),
                                                                msg.type_specific_data.size(), offset);

            auto originator_host = SshMessageCodec::read_string(msg.type_specific_data.data(),
                                                                msg.type_specific_data.size(), offset);
            if (!originator_host) {
                return std::nullopt;
            }

            if (offset + 4 > msg.type_specific_data.size()) {
                return std::nullopt;
            }
            uint32_t originator_port = SshMessageCodec::read_uint32(msg.type_specific_data.data(),
                                                                    msg.type_specific_data.size(), offset);

            if (offset != msg.type_specific_data.size()) {
                return std::nullopt;
            }

            if (target_port == 0 || target_port > 65535 || originator_port > 65535) {
                return std::nullopt;
            }

            DirectTcpipOpenData result;
            result.target_host = std::move(*target_host);
            result.target_port = static_cast<uint16_t>(target_port);
            result.originator_host = std::move(*originator_host);
            result.originator_port = static_cast<uint16_t>(originator_port);
            return result;
        }

        struct TcpipForwardRequestData
        {
            std::string bind_addr;
            uint16_t bind_port = 0;
        };

        std::optional<TcpipForwardRequestData> decode_tcpip_forward_request_data(const std::vector<uint8_t> &data)
        {
            size_t offset = 0;
            auto addr = SshMessageCodec::read_string(data.data(), data.size(), offset);
            if (!addr || offset + 4 > data.size()) {
                return std::nullopt;
            }

            uint32_t port = SshMessageCodec::read_uint32(data.data(), data.size(), offset);
            if (offset != data.size() || port > 65535) {
                return std::nullopt;
            }

            TcpipForwardRequestData result;
            result.bind_addr = std::move(*addr);
            result.bind_port = static_cast<uint16_t>(port);
            return result;
        }

        bool port_forwarding_enabled(const SshSession *session)
        {
            if (!session || !session->server()) {
                return true;
            }
            return session->server()->config().enable_port_forwarding;
        }

        bool host_port_matches_pattern(const std::string &host,
                                       uint16_t port,
                                       const std::string &pattern)
        {
            const auto colon = pattern.rfind(':');
            if (colon == std::string::npos || colon == 0 || colon + 1 >= pattern.size()) {
                return false;
            }

            const std::string pattern_host = pattern.substr(0, colon);
            const std::string pattern_port = pattern.substr(colon + 1);
            if (pattern_host != "*" && pattern_host != host) {
                return false;
            }

            if (pattern_port == "*") {
                return true;
            }

            uint32_t parsed_port = 0;
            for (char ch : pattern_port) {
                if (ch < '0' || ch > '9') {
                    return false;
                }
                parsed_port = parsed_port * 10 + static_cast<uint32_t>(ch - '0');
                if (parsed_port > 65535) {
                    return false;
                }
            }
            return parsed_port == port;
        }

        bool permitopen_allows_target(const SshSession *session,
                                      const std::string &host,
                                      uint16_t port)
        {
            if (!session) {
                return true;
            }
            const auto &permitopen = session->authorized_key_permitopen();
            if (permitopen.empty()) {
                return true;
            }
            for (const auto &pattern : permitopen) {
                if (host_port_matches_pattern(host, port, pattern)) {
                    return true;
                }
            }
            return false;
        }

        bool permitlisten_allows_target(const SshSession *session,
                                        const std::string &host,
                                        uint16_t port)
        {
            if (!session) {
                return true;
            }
            const auto &permitlisten = session->authorized_key_permitlisten();
            if (permitlisten.empty()) {
                return true;
            }
            for (const auto &pattern : permitlisten) {
                if (host_port_matches_pattern(host, port, pattern)) {
                    return true;
                }
            }
            return false;
        }

        SshChannel *resolve_recipient_channel(SshConnectionManager *manager, uint32_t recipient_channel)
        {
            if (!manager) {
                return nullptr;
            }

            auto *channel = manager->find_channel(recipient_channel);
            if (channel) {
                return channel;
            }

            return manager->find_channel_by_remote(recipient_channel);
        }
    }

    SshConnectionManager::SshConnectionManager(SshSession * session, uint32_t max_channels)
        : session_(session), max_channels_(max_channels), port_forwarding_(session)
    {
        port_forwarding_.set_connection_manager(this);
        if (session_) {
            port_forwarding_.set_runtime(session_->runtime());
        }
    }

    void SshConnectionManager::register_subsystem(const std::string & name, SubsystemFactory factory)
    {
        subsystem_factories_[name] = std::move(factory);
    }

    SshChannel *SshConnectionManager::create_channel(const std::string & type, uint32_t remote_channel_id,
                                                     uint32_t initial_window, uint32_t max_packet_size)
    {
        if (channel_limit_reached()) {
            return nullptr;
        }

        auto local_id = next_channel_id();
        auto channel = std::make_unique<SshChannel>(local_id, type, initial_window, max_packet_size);
        channel->set_remote_id(remote_channel_id);
        channel->set_remote_window(initial_window);
        channel->set_remote_max_packet(max_packet_size);
        channel->set_state(SshChannel::State::open);

        auto *ptr = yuan::base::owner_ptr(channel);
        std::lock_guard<std::mutex> lock(channels_mutex_);
        channels_.emplace(local_id, std::move(channel));
        return ptr;
    }

    SshChannel *SshConnectionManager::find_channel(uint32_t local_id)
    {
        std::lock_guard<std::mutex> lock(channels_mutex_);
        auto it = channels_.find(local_id);
        return it != channels_.end() ? yuan::base::owner_ptr(it->second) : nullptr;
    }

    SshChannel *SshConnectionManager::find_channel_by_remote(uint32_t remote_id)
    {
        std::lock_guard<std::mutex> lock(channels_mutex_);
        for (auto & [
                        id,
                        ch
                    ] : channels_) {
            if (ch->remote_id() == remote_id) {
                return yuan::base::owner_ptr(ch);
            }
        }
        return nullptr;
    }

    void SshConnectionManager::remove_channel(uint32_t local_id)
    {
        std::lock_guard<std::mutex> lock(channels_mutex_);
        channels_.erase(local_id);
    }

    void SshConnectionManager::close_all_channels()
    {
        std::lock_guard<std::mutex> lock(channels_mutex_);
        for (auto & [
                        id,
                        ch
                    ] : channels_) {
            ch->set_state(SshChannel::State::closed);
        }
        channels_.clear();
    }

    uint32_t SshConnectionManager::next_channel_id()
    {
        return next_local_id_.fetch_add(1, std::memory_order_relaxed);
    }

    uint32_t SshConnectionManager::channel_count() const
    {
        std::lock_guard<std::mutex> lock(channels_mutex_);
        return static_cast<uint32_t>(channels_.size());
    }

    bool SshConnectionManager::channel_limit_reached() const
    {
        return channel_count() >= max_channels_;
    }

    ByteBuffer SshConnectionManager::handle_channel_open(const SshChannelOpenMessage & msg, SshHandler * handler)
    {
        auto *effective_handler = handler ? handler : &SshHandler::default_handler();
        if (channel_limit_reached()) {
            return build_channel_open_failure(msg.sender_channel,
                                              SshChannelOpenFailureReason::SSH_OPEN_RESOURCE_SHORTAGE,
                                              "Too many channels");
        }

        auto local_id = next_channel_id();
        auto channel = std::make_unique<SshChannel>(local_id, msg.channel_type,
                                                    SSH_DEFAULT_WINDOW_SIZE, SSH_DEFAULT_MAX_PACKET_SIZE);
        channel->set_remote_id(msg.sender_channel);
        channel->set_remote_window(msg.initial_window_size);
        channel->set_remote_max_packet(msg.maximum_packet_size);
        channel->set_state(SshChannel::State::open);

        if (!effective_handler->on_channel_open(session_, msg.channel_type, yuan::base::owner_ptr(channel))) {
            return build_channel_open_failure(msg.sender_channel,
                                              SshChannelOpenFailureReason::SSH_OPEN_ADMINISTRATIVELY_PROHIBITED,
                                              "Channel open denied");
        }

        if (msg.channel_type == SSH_CHANNEL_SESSION) {
            // session channel authorization is handled by the configured/default handler
        } else if (msg.channel_type == SSH_CHANNEL_DIRECT_TCPIP) {
            if (!port_forwarding_enabled(session_)) {
                return build_channel_open_failure(msg.sender_channel,
                                                  SshChannelOpenFailureReason::SSH_OPEN_ADMINISTRATIVELY_PROHIBITED,
                                                  "Port forwarding disabled");
            }
            if (session_ && session_->authorized_key_no_port_forwarding()) {
                return build_channel_open_failure(msg.sender_channel,
                                                  SshChannelOpenFailureReason::SSH_OPEN_ADMINISTRATIVELY_PROHIBITED,
                                                  "Port forwarding denied by authorized_keys");
            }

            auto open_data = decode_direct_tcpip_open_data(msg);
            if (!open_data) {
                return build_channel_open_failure(msg.sender_channel,
                                                  SshChannelOpenFailureReason::SSH_OPEN_CONNECT_FAILED,
                                                  "Invalid direct-tcpip parameters");
            }

            if (!permitopen_allows_target(session_, open_data->target_host, open_data->target_port)) {
                return build_channel_open_failure(msg.sender_channel,
                                                  SshChannelOpenFailureReason::SSH_OPEN_ADMINISTRATIVELY_PROHIBITED,
                                                  "Target blocked by permitopen");
            }

            if (!effective_handler->on_direct_tcpip(session_, yuan::base::owner_ptr(channel),
                                                    open_data->target_host, open_data->target_port)) {
                return build_channel_open_failure(msg.sender_channel,
                                                  SshChannelOpenFailureReason::SSH_OPEN_ADMINISTRATIVELY_PROHIBITED,
                                                  "Direct TCP/IP forwarding denied");
            }

            auto direct_handler = std::make_unique<SshDirectTcpipHandler>(
                session_, open_data->target_host, open_data->target_port);
            channel->set_handler(std::move(direct_handler));
        } else if (msg.channel_type == SSH_CHANNEL_FORWARDED_TCPIP) {
            // server-initiated, should not be received
            return build_channel_open_failure(msg.sender_channel,
                                              SshChannelOpenFailureReason::SSH_OPEN_UNKNOWN_CHANNEL_TYPE,
                                              "Unexpected forwarded-tcpip channel open");
        } else {
            return build_channel_open_failure(msg.sender_channel,
                                              SshChannelOpenFailureReason::SSH_OPEN_UNKNOWN_CHANNEL_TYPE,
                                              "Unknown channel type: " + msg.channel_type);
        }

        auto *ptr = yuan::base::owner_ptr(channel);
        {
            std::lock_guard<std::mutex> lock(channels_mutex_);
            channels_.emplace(local_id, std::move(channel));
        }

        if (ptr->handler()) {
            ptr->handler()->on_open(ptr);
        }

        return build_channel_open_confirmation(msg.sender_channel, local_id,
                                               SSH_DEFAULT_WINDOW_SIZE, SSH_DEFAULT_MAX_PACKET_SIZE);
    }

    void SshConnectionManager::handle_channel_open_confirmation(const SshChannelOpenConfirmationMessage & msg)
    {
        auto *channel = find_channel(msg.recipient_channel);
        if (!channel) {
            return;
        }

        channel->set_remote_id(msg.sender_channel);
        channel->set_remote_window(msg.initial_window_size);
        channel->set_remote_max_packet(msg.maximum_packet_size);
        channel->set_state(SshChannel::State::open);

        if (channel->handler()) {
            channel->handler()->on_open(channel);
        }
    }

    void SshConnectionManager::handle_channel_open_failure(const SshChannelOpenFailureMessage & msg)
    {
        auto *channel = find_channel(msg.recipient_channel);
        if (!channel) {
            return;
        }

        channel->set_state(SshChannel::State::closed);
        remove_channel(msg.recipient_channel);
    }

    ByteBuffer SshConnectionManager::handle_channel_data(const SshChannelDataMessage & msg, SshHandler * handler)
    {
        auto *effective_handler = handler ? handler : &SshHandler::default_handler();
        auto *channel = resolve_recipient_channel(this, msg.recipient_channel);
        if (!channel) {
            return ByteBuffer();
        }

        if (channel->state() != SshChannel::State::open) {
            return ByteBuffer();
        }

        if (msg.data.size() > channel->local_max_packet()) {
            return ByteBuffer();
        }

        if (!channel->local_window_available(static_cast<uint32_t>(msg.data.size()))) {
            return ByteBuffer();
        }

        channel->consume_local_window(static_cast<uint32_t>(msg.data.size()));

        if (channel->handler()) {
            channel->handler()->on_data(channel, msg.data);
        }

        effective_handler->on_channel_data(session_, channel, msg.data);

        return maybe_adjust_window(channel);
    }

    ByteBuffer SshConnectionManager::handle_channel_extended_data(const SshChannelExtendedDataMessage & msg)
    {
        auto *channel = resolve_recipient_channel(this, msg.recipient_channel);
        if (!channel) {
            return ByteBuffer();
        }

        if (channel->state() != SshChannel::State::open) {
            return ByteBuffer();
        }

        if (!channel->local_window_available(static_cast<uint32_t>(msg.data.size()))) {
            return ByteBuffer();
        }

        channel->consume_local_window(static_cast<uint32_t>(msg.data.size()));

        return maybe_adjust_window(channel);
    }

    ByteBuffer SshConnectionManager::handle_channel_window_adjust(const SshChannelWindowAdjustMessage & msg)
    {
        auto *channel = resolve_recipient_channel(this, msg.recipient_channel);
        if (!channel) {
            return ByteBuffer();
        }

        channel->adjust_remote_window(msg.bytes_to_add);

        if (channel->handler()) {
            channel->handler()->on_window_adjust(channel, msg.bytes_to_add);
        }

        return ByteBuffer();
    }

    ByteBuffer SshConnectionManager::handle_channel_eof(const SshChannelEofMessage & msg)
    {
        auto *channel = resolve_recipient_channel(this, msg.recipient_channel);
        if (!channel) {
            return ByteBuffer();
        }

        if (channel->state() != SshChannel::State::open) {
            return ByteBuffer();
        }

        channel->set_state(SshChannel::State::eof);

        if (channel->handler()) {
            channel->handler()->on_eof(channel);
        }

        if (channel->terminal_session_state().subsystem_requested) {
            channel->set_state(SshChannel::State::closing);
            return build_channel_close(channel->remote_id());
        }

        return build_channel_eof(channel->remote_id());
    }

    ByteBuffer SshConnectionManager::handle_channel_close(const SshChannelCloseMessage & msg, SshHandler * handler)
    {
        auto *effective_handler = handler ? handler : &SshHandler::default_handler();
        auto *channel = resolve_recipient_channel(this, msg.recipient_channel);
        if (!channel) {
            return ByteBuffer();
        }

        if (channel->state() == SshChannel::State::closing ||
            channel->state() == SshChannel::State::closed) {
            auto local_id = channel->local_id();
            if (channel->handler()) {
                channel->handler()->on_close(channel);
            }
            effective_handler->on_channel_close(session_, channel);
            remove_channel(local_id);
            return ByteBuffer();
        }

        auto local_id = channel->local_id();
        auto remote_id = channel->remote_id();
        channel->set_state(SshChannel::State::closed);

        if (channel->handler()) {
            channel->handler()->on_close(channel);
        }

        effective_handler->on_channel_close(session_, channel);

        remove_channel(local_id);

        return build_channel_close(remote_id);
    }

    ByteBuffer SshConnectionManager::handle_channel_request(const SshChannelRequestMessage & msg, SshHandler * handler)
    {
        auto *effective_handler = handler ? handler : &SshHandler::default_handler();
        auto *channel = resolve_recipient_channel(this, msg.recipient_channel);
        if (!channel) {
            if (msg.want_reply) {
                return build_channel_failure(msg.recipient_channel);
            }
            return ByteBuffer();
        }

        if (channel->state() != SshChannel::State::open) {
            if (msg.want_reply) {
                return build_channel_failure(channel->remote_id());
            }
            return ByteBuffer();
        }

        bool success = false;
        bool handled_by_channel_handler = false;

        if (channel->handler()) {
            success = channel->handler()->on_request(channel, msg.request_type, msg.request_specific_data);
            handled_by_channel_handler = success;
        }

        if (!handled_by_channel_handler && msg.request_type == "exec") {
                if (!channel->mark_command_started()) {
                    success = false;
                } else {
                SshExecRequestData exec_data;
                size_t offset = 0;
                auto cmd = SshMessageCodec::read_string(msg.request_specific_data.data(),
                                                        msg.request_specific_data.size(), offset);
                if (cmd) {
                    if (session_ && session_->has_authorized_key_forced_command()) {
                        exec_data.command = session_->authorized_key_forced_command();
                    } else {
                        exec_data.command = std::move(*cmd);
                    }
                    success = effective_handler->on_exec_request(session_, channel, exec_data.command);
                    if (success) {
                        auto &terminal_state = channel->terminal_session_state();
                        terminal_state.exec_requested = true;
                        terminal_state.exec_command = exec_data.command;
                    }
                }
                }
        } else if (msg.request_type == "subsystem") {
                if (!channel->mark_command_started()) {
                    success = false;
                } else {
                SshSubsystemRequestData sub_data;
                size_t offset = 0;
                auto name = SshMessageCodec::read_string(msg.request_specific_data.data(),
                                                         msg.request_specific_data.size(), offset);
                if (name) {
                    sub_data.subsystem_name = std::move(*name);
                    auto it = subsystem_factories_.find(sub_data.subsystem_name);
                    bool builtin_allowed = it != subsystem_factories_.end();
                    success = builtin_allowed ||
                              effective_handler->on_subsystem_request(session_, channel, sub_data.subsystem_name);

                    if (success) {
                        channel->terminal_session_state().subsystem_requested = true;
                        if (it != subsystem_factories_.end()) {
                            auto subsystem_handler = it->second();
                            auto *raw = yuan::base::owner_ptr(subsystem_handler);
                            channel->set_handler(std::move(subsystem_handler));
                            raw->on_open(channel);
                        }
                    }
                }
                }
        } else if (msg.request_type == "pty-req") {
                if (session_ && session_->authorized_key_no_pty()) {
                    success = false;
                } else if (channel->command_started()) {
                    success = false;
                } else if (!channel->mark_pty_requested()) {
                    success = false;
                } else {
                SshPtyRequestData pty_data;
                size_t offset = 0;
                auto term = SshMessageCodec::read_string(msg.request_specific_data.data(),
                                                         msg.request_specific_data.size(), offset);
                if (term) {
                    pty_data.term_env = std::move(*term);
                }
                if (offset + 16 <= msg.request_specific_data.size()) {
                    pty_data.terminal_width = SshMessageCodec::read_uint32(msg.request_specific_data.data(),
                                                                           msg.request_specific_data.size(), offset);
                    pty_data.terminal_height = SshMessageCodec::read_uint32(msg.request_specific_data.data(),
                                                                            msg.request_specific_data.size(), offset);
                    pty_data.terminal_width_pixels = SshMessageCodec::read_uint32(msg.request_specific_data.data(),
                                                                                  msg.request_specific_data.size(), offset);
                    pty_data.terminal_height_pixels = SshMessageCodec::read_uint32(msg.request_specific_data.data(),
                                                                                   msg.request_specific_data.size(), offset);
                }
                auto modes = SshMessageCodec::read_string(msg.request_specific_data.data(),
                                                          msg.request_specific_data.size(), offset);
                if (modes) {
                    pty_data.terminal_modes.assign(modes->begin(), modes->end());
                }
                success = effective_handler->on_pty_request(session_, channel,
                                                            pty_data.term_env, pty_data.terminal_width, pty_data.terminal_height,
                                                            pty_data.terminal_width_pixels, pty_data.terminal_height_pixels,
                                                            pty_data.terminal_modes);
                auto &terminal_state = channel->terminal_session_state();
                terminal_state.has_pty_request = success;
                if (success) {
                    terminal_state.spec.term_env = pty_data.term_env;
                    terminal_state.spec.width = pty_data.terminal_width;
                    terminal_state.spec.height = pty_data.terminal_height;
                    terminal_state.spec.pixel_width = pty_data.terminal_width_pixels;
                    terminal_state.spec.pixel_height = pty_data.terminal_height_pixels;
                    terminal_state.spec.terminal_modes = pty_data.terminal_modes;
                }
                }
        } else if (msg.request_type == "shell") {
                if (!channel->mark_command_started()) {
                    success = false;
                } else {
                success = effective_handler->on_shell_request(session_, channel);
                channel->terminal_session_state().interactive_shell_requested = success;
                }
        } else if (msg.request_type == "env") {
                if (channel->command_started()) {
                    success = false;
                } else {
                SshEnvRequestData env_data;
                size_t offset = 0;
                auto var_name = SshMessageCodec::read_string(msg.request_specific_data.data(),
                                                             msg.request_specific_data.size(), offset);
                auto var_value = SshMessageCodec::read_string(msg.request_specific_data.data(),
                                                              msg.request_specific_data.size(), offset);
                if (var_name && var_value) {
                    env_data.variable_name = std::move(*var_name);
                    env_data.variable_value = std::move(*var_value);
                    success = effective_handler->on_env_request(session_, channel,
                                                                env_data.variable_name, env_data.variable_value);
                }
                }
        } else if (msg.request_type == "window-change") {
                SshWindowChangeData wc_data;
                size_t offset = 0;
                if (offset + 16 <= msg.request_specific_data.size()) {
                    wc_data.terminal_width = SshMessageCodec::read_uint32(msg.request_specific_data.data(),
                                                                          msg.request_specific_data.size(), offset);
                    wc_data.terminal_height = SshMessageCodec::read_uint32(msg.request_specific_data.data(),
                                                                           msg.request_specific_data.size(), offset);
                    wc_data.terminal_width_pixels = SshMessageCodec::read_uint32(msg.request_specific_data.data(),
                                                                                 msg.request_specific_data.size(), offset);
                    wc_data.terminal_height_pixels = SshMessageCodec::read_uint32(msg.request_specific_data.data(),
                                                                                   msg.request_specific_data.size(), offset);
                }
                effective_handler->on_window_change(session_, channel,
                                                    wc_data.terminal_width, wc_data.terminal_height,
                                                    wc_data.terminal_width_pixels, wc_data.terminal_height_pixels);
                success = true;
        } else if (msg.request_type == "signal") {
                SshSignalData sig_data;
                size_t offset = 0;
                auto sig_name = SshMessageCodec::read_string(msg.request_specific_data.data(),
                                                             msg.request_specific_data.size(), offset);
                if (sig_name) {
                    sig_data.signal_name = std::move(*sig_name);
                    effective_handler->on_signal(session_, channel, sig_data.signal_name);
                    success = true;
                }
        } else if (msg.request_type == "x11-req") {
                if (session_ && session_->authorized_key_no_x11_forwarding()) {
                    success = false;
                } else {
                    success = effective_handler->on_x11_forward(session_, channel, "", "", 0);
                }
        } else if (msg.request_type == "auth-agent-req@openssh.com") {
                if (session_ && session_->authorized_key_no_agent_forwarding()) {
                    success = false;
                } else {
                    success = effective_handler->on_agent_forward(session_, channel);
                }
        } else {
                success = effective_handler->on_channel_request(session_, channel,
                                                                msg.request_type, msg.request_specific_data);
        }

        if (msg.want_reply) {
            return success ? build_channel_success(channel->remote_id())
                           : build_channel_failure(channel->remote_id());
        }

        return ByteBuffer();
    }

    ByteBuffer SshConnectionManager::handle_global_request(const SshGlobalRequestMessage & msg, SshHandler * handler)
    {
        auto *effective_handler = handler ? handler : &SshHandler::default_handler();
        if (msg.request_name == "tcpip-forward") {
            if (!port_forwarding_enabled(session_)) {
                if (msg.want_reply) {
                    return build_request_failure();
                }
                return ByteBuffer();
            }
            if (session_ && session_->authorized_key_no_port_forwarding()) {
                if (msg.want_reply) {
                    return build_request_failure();
                }
                return ByteBuffer();
            }

            auto request_data = decode_tcpip_forward_request_data(msg.request_specific_data);
            if (request_data) {
                if (!permitlisten_allows_target(session_, request_data->bind_addr, request_data->bind_port)) {
                    if (msg.want_reply) {
                        return build_request_failure();
                    }
                    return ByteBuffer();
                }
                const uint16_t handler_allocated = effective_handler->on_tcpip_forward(session_,
                                                                                        request_data->bind_addr,
                                                                                        request_data->bind_port);
                if (handler_allocated == 0) {
                    if (msg.want_reply) {
                        return build_request_failure();
                    }
                    return ByteBuffer();
                }

                uint16_t allocated = 0;
                const bool accepted = port_forwarding_.handle_tcpip_forward(request_data->bind_addr,
                                                                             handler_allocated,
                                                                             allocated);
                if (!accepted) {
                    if (msg.want_reply) {
                        return build_request_failure();
                    }
                    return ByteBuffer();
                }

                auto remote_listener = effective_handler->on_forwarded_tcpip_listener(
                    session_,
                    request_data->bind_addr,
                    allocated > 0 ? allocated : request_data->bind_port);
                if (remote_listener) {
                    std::string forward_key = request_data->bind_addr + ":" + std::to_string(allocated);
                    if (!port_forwarding_.attach_remote_forward_listener(forward_key, std::move(remote_listener))) {
                        port_forwarding_.handle_cancel_tcpip_forward(request_data->bind_addr, allocated);
                        if (msg.want_reply) {
                            return build_request_failure();
                        }
                        return ByteBuffer();
                    }
                }
                if (allocated > 0) {
                    if (msg.want_reply) {
                        ByteBuffer resp;
                        resp.append_u8(static_cast<uint8_t>(SshMessageType::SSH_MSG_REQUEST_SUCCESS));
                        SshMessageCodec::write_uint32(resp, allocated);
                        return resp;
                    }
                    return ByteBuffer();
                }
            }

            if (msg.want_reply) {
                return build_request_failure();
            }
            return ByteBuffer();
        }

        if (msg.request_name == "cancel-tcpip-forward") {
            if (!port_forwarding_enabled(session_)) {
                if (msg.want_reply) {
                    return build_request_failure();
                }
                return ByteBuffer();
            }
            if (session_ && session_->authorized_key_no_port_forwarding()) {
                if (msg.want_reply) {
                    return build_request_failure();
                }
                return ByteBuffer();
            }

            auto request_data = decode_tcpip_forward_request_data(msg.request_specific_data);
            if (request_data) {
                const bool cancelled = port_forwarding_.handle_cancel_tcpip_forward(request_data->bind_addr,
                                                                                     request_data->bind_port);
                if (!cancelled && msg.want_reply) {
                    return build_request_failure();
                }
                effective_handler->on_cancel_tcpip_forward(session_,
                                                           request_data->bind_addr,
                                                           request_data->bind_port);
            } else if (msg.want_reply) {
                return build_request_failure();
            }

            if (msg.want_reply) {
                return build_request_success();
            }
            return ByteBuffer();
        }

        if (msg.request_name == "keepalive@openssh.com") {
            if (msg.want_reply) {
                return build_request_success();
            }
            return ByteBuffer();
        }

        if (msg.request_name == "no-more-sessions@openssh.com") {
            if (msg.want_reply) {
                return build_request_success();
            }
            return ByteBuffer();
        }

        bool handled = effective_handler->on_global_request(session_, msg.request_name, msg.request_specific_data);

        if (msg.want_reply) {
            return handled ? build_request_success() : build_request_failure();
        }

        return ByteBuffer();
    }

    ByteBuffer SshConnectionManager::build_channel_open_confirmation(uint32_t recipient, uint32_t sender,
                                                                     uint32_t window, uint32_t max_pkt) const
    {
        SshChannelOpenConfirmationMessage msg;
        msg.recipient_channel = recipient;
        msg.sender_channel = sender;
        msg.initial_window_size = window;
        msg.maximum_packet_size = max_pkt;
        return SshMessageCodec::encode_channel_open_confirmation(msg);
    }

    ByteBuffer SshConnectionManager::build_channel_open_failure(uint32_t recipient,
                                                                SshChannelOpenFailureReason reason,
                                                                const std::string & description) const
    {
        SshChannelOpenFailureMessage msg;
        msg.recipient_channel = recipient;
        msg.reason_code = static_cast<uint32_t>(reason);
        msg.description = description;
        msg.language = "en";
        return SshMessageCodec::encode_channel_open_failure(msg);
    }

    ByteBuffer SshConnectionManager::build_channel_data(uint32_t recipient, const std::vector<uint8_t> & data) const
    {
        SshChannelDataMessage msg;
        msg.recipient_channel = recipient;
        msg.data = data;
        return SshMessageCodec::encode_channel_data(msg);
    }

    ByteBuffer SshConnectionManager::build_channel_extended_data(uint32_t recipient, uint32_t type,
                                                                 const std::vector<uint8_t> & data) const
    {
        SshChannelExtendedDataMessage msg;
        msg.recipient_channel = recipient;
        msg.data_type_code = type;
        msg.data = data;
        return SshMessageCodec::encode_channel_extended_data(msg);
    }

    ByteBuffer SshConnectionManager::build_channel_eof(uint32_t recipient) const
    {
        SshChannelEofMessage msg;
        msg.recipient_channel = recipient;
        return SshMessageCodec::encode_channel_eof(msg);
    }

    ByteBuffer SshConnectionManager::build_channel_close(uint32_t recipient) const
    {
        SshChannelCloseMessage msg;
        msg.recipient_channel = recipient;
        return SshMessageCodec::encode_channel_close(msg);
    }

    ByteBuffer SshConnectionManager::build_window_adjust(uint32_t recipient, uint32_t bytes) const
    {
        SshChannelWindowAdjustMessage msg;
        msg.recipient_channel = recipient;
        msg.bytes_to_add = bytes;
        return SshMessageCodec::encode_channel_window_adjust(msg);
    }

    ByteBuffer SshConnectionManager::build_channel_success(uint32_t recipient) const
    {
        SshChannelSuccessMessage msg;
        msg.recipient_channel = recipient;
        return SshMessageCodec::encode_channel_success(msg);
    }

    ByteBuffer SshConnectionManager::build_channel_failure(uint32_t recipient) const
    {
        SshChannelFailureMessage msg;
        msg.recipient_channel = recipient;
        return SshMessageCodec::encode_channel_failure(msg);
    }

    ByteBuffer SshConnectionManager::build_channel_exit_status(uint32_t recipient, uint32_t exit_status) const
    {
        ByteBuffer buf;
        buf.append_u8(static_cast<uint8_t>(SshMessageType::SSH_MSG_CHANNEL_REQUEST));
        SshMessageCodec::write_uint32(buf, recipient);
        SshMessageCodec::write_string(buf, "exit-status");
        SshMessageCodec::write_boolean(buf, false);
        SshMessageCodec::write_uint32(buf, exit_status);
        return buf;
    }

    ByteBuffer SshConnectionManager::build_channel_exit_signal(uint32_t recipient,
                                                               const std::string &signal_name,
                                                               bool core_dumped,
                                                               const std::string &error_message,
                                                               const std::string &language_tag) const
    {
        ByteBuffer buf;
        buf.append_u8(static_cast<uint8_t>(SshMessageType::SSH_MSG_CHANNEL_REQUEST));
        SshMessageCodec::write_uint32(buf, recipient);
        SshMessageCodec::write_string(buf, "exit-signal");
        SshMessageCodec::write_boolean(buf, false);
        SshMessageCodec::write_string(buf, signal_name);
        SshMessageCodec::write_boolean(buf, core_dumped);
        SshMessageCodec::write_string(buf, error_message);
        SshMessageCodec::write_string(buf, language_tag);
        return buf;
    }

    ByteBuffer SshConnectionManager::build_request_success(const std::vector<uint8_t> & data) const
    {
        ByteBuffer buf;
        buf.append_u8(static_cast<uint8_t>(SshMessageType::SSH_MSG_REQUEST_SUCCESS));
        if (!data.empty()) {
            buf.append(data.data(), data.size());
        }
        return buf;
    }

    ByteBuffer SshConnectionManager::build_request_failure() const
    {
        ByteBuffer buf;
        buf.append_u8(static_cast<uint8_t>(SshMessageType::SSH_MSG_REQUEST_FAILURE));
        return buf;
    }

    ByteBuffer SshConnectionManager::build_forwarded_tcpip_channel_open(const std::string & connected_address,
                                                                         uint32_t connected_port,
                                                                         const std::string & originator_address,
                                                                         uint32_t originator_port,
                                                                         uint32_t initial_window,
                                                                         uint32_t maximum_packet_size)
    {
        SshChannelOpenMessage msg;
        msg.channel_type = SSH_CHANNEL_FORWARDED_TCPIP;
        msg.sender_channel = next_channel_id();
        msg.initial_window_size = initial_window;
        msg.maximum_packet_size = maximum_packet_size;

        ByteBuffer payload;
        SshMessageCodec::write_string(payload, connected_address);
        SshMessageCodec::write_uint32(payload, connected_port);
        SshMessageCodec::write_string(payload, originator_address);
        SshMessageCodec::write_uint32(payload, originator_port);
        auto span = payload.readable_span();
        msg.type_specific_data.assign(
            reinterpret_cast<const uint8_t *>(span.data()),
            reinterpret_cast<const uint8_t *>(span.data()) + span.size());

        return SshMessageCodec::encode_channel_open(msg);
    }

    std::optional<uint32_t> SshConnectionManager::open_forwarded_tcpip_channel(const std::string & connected_address,
                                                                                uint32_t connected_port,
                                                                                const std::string & originator_address,
                                                                                uint32_t originator_port,
                                                                                ByteBuffer & packet_out,
                                                                                uint32_t initial_window,
                                                                                uint32_t maximum_packet_size)
    {
        if (channel_limit_reached()) {
            return std::nullopt;
        }

        const uint32_t local_id = next_channel_id();
        auto channel = std::make_unique<SshChannel>(
            local_id,
            SSH_CHANNEL_FORWARDED_TCPIP,
            initial_window,
            maximum_packet_size);
        channel->set_state(SshChannel::State::opening);
        channel->set_remote_window(initial_window);
        channel->set_remote_max_packet(maximum_packet_size);

        {
            std::lock_guard<std::mutex> lock(channels_mutex_);
            channels_.emplace(local_id, std::move(channel));
        }

        SshChannelOpenMessage msg;
        msg.channel_type = SSH_CHANNEL_FORWARDED_TCPIP;
        msg.sender_channel = local_id;
        msg.initial_window_size = initial_window;
        msg.maximum_packet_size = maximum_packet_size;

        ByteBuffer payload;
        SshMessageCodec::write_string(payload, connected_address);
        SshMessageCodec::write_uint32(payload, connected_port);
        SshMessageCodec::write_string(payload, originator_address);
        SshMessageCodec::write_uint32(payload, originator_port);
        auto span = payload.readable_span();
        msg.type_specific_data.assign(
            reinterpret_cast<const uint8_t *>(span.data()),
            reinterpret_cast<const uint8_t *>(span.data()) + span.size());

        packet_out = SshMessageCodec::encode_channel_open(msg);
        return local_id;
    }

    bool SshConnectionManager::register_forwarded_tcpip_handler(uint32_t local_channel_id,
                                                                std::unique_ptr<SshChannelHandler> handler)
    {
        if (!handler) {
            return false;
        }

        SshChannel *channel = nullptr;
        {
            std::lock_guard<std::mutex> lock(channels_mutex_);
            auto it = channels_.find(local_channel_id);
            if (it == channels_.end()) {
                return false;
            }
            channel = yuan::base::owner_ptr(it->second);
            channel->set_handler(std::move(handler));
        }
        return channel != nullptr;
    }

    void SshConnectionManager::set_runtime(coroutine::RuntimeView runtime)
    {
        port_forwarding_.set_runtime(runtime);
    }

    void SshConnectionManager::poll_async_tasks()
    {
        port_forwarding_.poll_remote_forward_accepts();
    }

    bool SshConnectionManager::has_remote_forwards() const
    {
        return port_forwarding_.has_remote_forwards();
    }

    ByteBuffer SshConnectionManager::maybe_adjust_window(SshChannel * channel)
    {
        uint32_t current = channel->local_window();
        uint32_t threshold = SSH_DEFAULT_WINDOW_SIZE / SSH_WINDOW_ADJUST_THRESHOLD_DIV;
        if (current < threshold) {
            uint32_t to_add = SSH_DEFAULT_WINDOW_SIZE - current;
            channel->adjust_local_window(to_add);
            return build_window_adjust(channel->remote_id(), to_add);
        }
        return ByteBuffer();
    }

    std::vector<ByteBuffer> SshConnectionManager::drain_channel_pending_data()
    {
        std::vector<ByteBuffer> result;
        std::lock_guard<std::mutex> lock(channels_mutex_);
        for (auto & [
                        id,
                        ch
                    ] : channels_) {
            if (ch->state() != SshChannel::State::open) {
                continue;
            }
            while (ch->has_pending_data()) {
                uint32_t remote_win = ch->remote_window();
                if (remote_win == 0) {
                    break;
                }
                uint32_t max_pkt = ch->remote_max_packet();
                uint32_t chunk_size = std::min({ remote_win, max_pkt, static_cast<uint32_t>(256 * 1024) });
                auto data = ch->dequeue_pending(chunk_size);
                if (data.empty()) {
                    break;
                }
                ch->consume_remote_window(static_cast<uint32_t>(data.size()));
                result.push_back(build_channel_data(ch->remote_id(), data));
            }
        }
        return result;
    }
}
