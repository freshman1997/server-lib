#include "connection/ssh_connection_manager.h"
#include "connection/ssh_global_request.h"
#include "connection/ssh_port_forwarding.h"
#include "connection/ssh_direct_tcpip_handler.h"
#include "protocol/ssh_message_codec.h"
#include "ssh_handler.h"
#include "ssh_config.h"
#include "ssh_session.h"
#include <algorithm>

namespace yuan::net::ssh
{
    SshConnectionManager::SshConnectionManager(SshSession * session, uint32_t max_channels)
        : session_(session), max_channels_(max_channels)
    {
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

        auto *ptr = channel.get();
        std::lock_guard<std::mutex> lock(channels_mutex_);
        channels_.emplace(local_id, std::move(channel));
        return ptr;
    }

    SshChannel *SshConnectionManager::find_channel(uint32_t local_id)
    {
        std::lock_guard<std::mutex> lock(channels_mutex_);
        auto it = channels_.find(local_id);
        return it != channels_.end() ? it->second.get() : nullptr;
    }

    SshChannel *SshConnectionManager::find_channel_by_remote(uint32_t remote_id)
    {
        std::lock_guard<std::mutex> lock(channels_mutex_);
        for (auto & [
                        id,
                        ch
                    ] : channels_) {
            if (ch->remote_id() == remote_id) {
                return ch.get();
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
        if (channel_limit_reached()) {
            return build_channel_open_failure(msg.sender_channel,
                                              SshChannelOpenFailureReason::SSH_OPEN_RESOURCE_SHORTAGE,
                                              "Too many channels");
        }

        if (handler) {
            auto local_id = next_channel_id();
            auto channel = std::make_unique<SshChannel>(local_id, msg.channel_type,
                                                        SSH_DEFAULT_WINDOW_SIZE, SSH_DEFAULT_MAX_PACKET_SIZE);
            channel->set_remote_id(msg.sender_channel);
            channel->set_remote_window(msg.initial_window_size);
            channel->set_remote_max_packet(msg.maximum_packet_size);
            channel->set_state(SshChannel::State::open);

            bool allowed = handler->on_channel_open(session_, msg.channel_type, channel.get());
            if (!allowed) {
                return build_channel_open_failure(msg.sender_channel,
                                                  SshChannelOpenFailureReason::SSH_OPEN_ADMINISTRATIVELY_PROHIBITED,
                                                  "Channel open denied");
            }

            if (msg.channel_type == SSH_CHANNEL_SESSION) {
                // session channel - no additional setup
            } else if (msg.channel_type == SSH_CHANNEL_DIRECT_TCPIP) {
                size_t offset = 0;
                auto host = SshMessageCodec::read_string(msg.type_specific_data.data(),
                                                         msg.type_specific_data.size(), offset);
                uint32_t port = 0;
                if (offset + 4 <= msg.type_specific_data.size()) {
                    port = SshMessageCodec::read_uint32(msg.type_specific_data.data(),
                                                        msg.type_specific_data.size(), offset);
                }
                if (!host || port == 0) {
                    return build_channel_open_failure(msg.sender_channel,
                                                      SshChannelOpenFailureReason::SSH_OPEN_CONNECT_FAILED,
                                                      "Invalid direct-tcpip parameters");
                }

                bool allowed = false;
                if (handler) {
                    allowed = handler->on_direct_tcpip(session_, channel.get(), *host, static_cast<uint16_t>(port));
                }
                if (!allowed) {
                    return build_channel_open_failure(msg.sender_channel,
                                                      SshChannelOpenFailureReason::SSH_OPEN_ADMINISTRATIVELY_PROHIBITED,
                                                      "Direct TCP/IP forwarding denied");
                }

                auto direct_handler = std::make_unique<SshDirectTcpipHandler>(
                    session_, *host, static_cast<uint16_t>(port));
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

            auto *ptr = channel.get();
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

        return build_channel_open_failure(msg.sender_channel,
                                          SshChannelOpenFailureReason::SSH_OPEN_ADMINISTRATIVELY_PROHIBITED,
                                          "No handler");
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
        auto *channel = find_channel_by_remote(msg.recipient_channel);
        if (!channel) {
            return {};
        }

        if (channel->state() != SshChannel::State::open) {
            return {};
        }

        if (msg.data.size() > channel->local_max_packet()) {
            return {};
        }

        if (!channel->local_window_available(static_cast<uint32_t>(msg.data.size()))) {
            return {};
        }

        channel->consume_local_window(static_cast<uint32_t>(msg.data.size()));

        if (channel->handler()) {
            channel->handler()->on_data(channel, msg.data);
        }

        if (handler) {
            handler->on_channel_data(session_, channel, msg.data);
        }

        return maybe_adjust_window(channel);
    }

    ByteBuffer SshConnectionManager::handle_channel_extended_data(const SshChannelExtendedDataMessage & msg)
    {
        auto *channel = find_channel_by_remote(msg.recipient_channel);
        if (!channel) {
            return {};
        }

        if (channel->state() != SshChannel::State::open) {
            return {};
        }

        if (!channel->local_window_available(static_cast<uint32_t>(msg.data.size()))) {
            return {};
        }

        channel->consume_local_window(static_cast<uint32_t>(msg.data.size()));

        return maybe_adjust_window(channel);
    }

    ByteBuffer SshConnectionManager::handle_channel_window_adjust(const SshChannelWindowAdjustMessage & msg)
    {
        auto *channel = find_channel_by_remote(msg.recipient_channel);
        if (!channel) {
            return {};
        }

        channel->adjust_remote_window(msg.bytes_to_add);

        if (channel->handler()) {
            channel->handler()->on_window_adjust(channel, msg.bytes_to_add);
        }

        return {};
    }

    ByteBuffer SshConnectionManager::handle_channel_eof(const SshChannelEofMessage & msg)
    {
        auto *channel = find_channel_by_remote(msg.recipient_channel);
        if (!channel) {
            return {};
        }

        if (channel->state() != SshChannel::State::open) {
            return {};
        }

        channel->set_state(SshChannel::State::eof);

        if (channel->handler()) {
            channel->handler()->on_eof(channel);
        }

        return build_channel_eof(channel->remote_id());
    }

    ByteBuffer SshConnectionManager::handle_channel_close(const SshChannelCloseMessage & msg, SshHandler * handler)
    {
        auto *channel = find_channel_by_remote(msg.recipient_channel);
        if (!channel) {
            return {};
        }

        auto local_id = channel->local_id();
        channel->set_state(SshChannel::State::closed);

        if (channel->handler()) {
            channel->handler()->on_close(channel);
        }

        if (handler) {
            handler->on_channel_close(session_, channel);
        }

        remove_channel(local_id);

        return build_channel_close(channel->remote_id());
    }

    ByteBuffer SshConnectionManager::handle_channel_request(const SshChannelRequestMessage & msg, SshHandler * handler)
    {
        auto *channel = find_channel_by_remote(msg.recipient_channel);
        if (!channel) {
            if (msg.want_reply) {
                return build_channel_failure(msg.recipient_channel);
            }
            return {};
        }

        bool success = false;

        if (channel->handler()) {
            success = channel->handler()->on_request(channel, msg.request_type, msg.request_specific_data);
        }

        if (handler) {
            if (msg.request_type == "exec") {
                SshExecRequestData exec_data;
                size_t offset = 0;
                auto cmd = SshMessageCodec::read_string(msg.request_specific_data.data(),
                                                        msg.request_specific_data.size(), offset);
                if (cmd) {
                    exec_data.command = std::move(*cmd);
                    success = handler->on_exec_request(session_, channel, exec_data.command);
                }
            } else if (msg.request_type == "subsystem") {
                SshSubsystemRequestData sub_data;
                size_t offset = 0;
                auto name = SshMessageCodec::read_string(msg.request_specific_data.data(),
                                                         msg.request_specific_data.size(), offset);
                if (name) {
                    sub_data.subsystem_name = std::move(*name);
                    success = handler->on_subsystem_request(session_, channel, sub_data.subsystem_name);

                    if (success) {
                        auto it = subsystem_factories_.find(sub_data.subsystem_name);
                        if (it != subsystem_factories_.end()) {
                            auto subsystem_handler = it->second();
                            auto *raw = subsystem_handler.get();
                            channel->set_handler(std::move(subsystem_handler));
                            raw->on_open(channel);
                        }
                    }
                }
            } else if (msg.request_type == "pty-req") {
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
                auto modes = SshMessageCodec::read_raw(msg.request_specific_data.data(),
                                                       msg.request_specific_data.size(), offset, msg.request_specific_data.size() - offset);
                if (modes) {
                    pty_data.terminal_modes = std::move(*modes);
                }
                success = handler->on_pty_request(session_, channel,
                                                  pty_data.term_env, pty_data.terminal_width, pty_data.terminal_height,
                                                  pty_data.terminal_width_pixels, pty_data.terminal_height_pixels,
                                                  pty_data.terminal_modes);
            } else if (msg.request_type == "shell") {
                success = handler->on_shell_request(session_, channel);
            } else if (msg.request_type == "env") {
                SshEnvRequestData env_data;
                size_t offset = 0;
                auto var_name = SshMessageCodec::read_string(msg.request_specific_data.data(),
                                                             msg.request_specific_data.size(), offset);
                auto var_value = SshMessageCodec::read_string(msg.request_specific_data.data(),
                                                              msg.request_specific_data.size(), offset);
                if (var_name && var_value) {
                    env_data.variable_name = std::move(*var_name);
                    env_data.variable_value = std::move(*var_value);
                    success = handler->on_env_request(session_, channel,
                                                      env_data.variable_name, env_data.variable_value);
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
                handler->on_window_change(session_, channel,
                                          wc_data.terminal_width, wc_data.terminal_height,
                                          wc_data.terminal_width_pixels, wc_data.terminal_height_pixels);
            } else if (msg.request_type == "signal") {
                SshSignalData sig_data;
                size_t offset = 0;
                auto sig_name = SshMessageCodec::read_string(msg.request_specific_data.data(),
                                                             msg.request_specific_data.size(), offset);
                if (sig_name) {
                    sig_data.signal_name = std::move(*sig_name);
                    handler->on_signal(session_, channel, sig_data.signal_name);
                }
            } else if (msg.request_type == "x11-req") {
                success = handler->on_x11_forward(session_, channel, "", "", 0);
            } else if (msg.request_type == "auth-agent-req@openssh.com") {
                success = handler->on_agent_forward(session_, channel);
            } else {
                success = handler->on_channel_request(session_, channel,
                                                      msg.request_type, msg.request_specific_data);
            }
        }

        if (msg.want_reply) {
            return success ? build_channel_success(channel->remote_id())
                           : build_channel_failure(channel->remote_id());
        }

        return {};
    }

    ByteBuffer SshConnectionManager::handle_global_request(const SshGlobalRequestMessage & msg, SshHandler * handler)
    {
        if (msg.request_name == "tcpip-forward") {
            size_t offset = 0;
            auto addr = SshMessageCodec::read_string(msg.request_specific_data.data(),
                                                     msg.request_specific_data.size(), offset);
            uint32_t port = 0;
            if (offset + 4 <= msg.request_specific_data.size()) {
                port = SshMessageCodec::read_uint32(msg.request_specific_data.data(),
                                                    msg.request_specific_data.size(), offset);
            }

            if (handler && addr) {
                uint16_t allocated = handler->on_tcpip_forward(session_, *addr, static_cast<uint16_t>(port));
                if (allocated > 0) {
                    if (msg.want_reply) {
                        ByteBuffer resp;
                        SshMessageCodec::write_uint32(resp, static_cast<uint32_t>(
                                                                static_cast<SshMessageType>(SSH_MSG_REQUEST_SUCCESS)));
                        SshMessageCodec::write_uint32(resp, allocated);
                        return resp;
                    }
                    return {};
                }
            }

            if (msg.want_reply) {
                return build_request_failure();
            }
            return {};
        }

        if (msg.request_name == "cancel-tcpip-forward") {
            size_t offset = 0;
            auto addr = SshMessageCodec::read_string(msg.request_specific_data.data(),
                                                     msg.request_specific_data.size(), offset);
            uint32_t port = 0;
            if (offset + 4 <= msg.request_specific_data.size()) {
                port = SshMessageCodec::read_uint32(msg.request_specific_data.data(),
                                                    msg.request_specific_data.size(), offset);
            }

            if (handler && addr) {
                handler->on_cancel_tcpip_forward(session_, *addr, static_cast<uint16_t>(port));
            }

            if (msg.want_reply) {
                return build_request_success();
            }
            return {};
        }

        bool handled = false;
        if (handler) {
            handled = handler->on_global_request(session_, msg.request_name, msg.request_specific_data);
        }

        if (msg.want_reply) {
            return handled ? build_request_success() : build_request_failure();
        }

        return {};
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

    ByteBuffer SshConnectionManager::build_request_success(const std::vector<uint8_t> & data) const
    {
        ByteBuffer buf;
        SshMessageCodec::write_uint32(buf, static_cast<uint32_t>(
                                               static_cast<SshMessageType>(SSH_MSG_REQUEST_SUCCESS)));
        if (!data.empty()) {
            SshMessageCodec::write_raw(buf, data.data(), data.size());
        }
        return buf;
    }

    ByteBuffer SshConnectionManager::build_request_failure() const
    {
        ByteBuffer buf;
        SshMessageCodec::write_uint32(buf, static_cast<uint32_t>(
                                               static_cast<SshMessageType>(SSH_MSG_REQUEST_FAILURE)));
        return buf;
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
        return {};
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
