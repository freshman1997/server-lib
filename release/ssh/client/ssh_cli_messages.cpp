#include "ssh_cli_messages.h"

#include "protocol/ssh_message_codec.h"
#include "ssh_cli_terminal.h"

namespace yuan::release_ssh::client
{
    namespace
    {
        std::vector<uint8_t> buffer_to_bytes(const yuan::buffer::ByteBuffer &data)
        {
            return {
                reinterpret_cast<const uint8_t *>(data.read_ptr()),
                reinterpret_cast<const uint8_t *>(data.read_ptr()) + data.readable_bytes()
            };
        }
    }

    std::vector<uint8_t> make_exec_request_data(const std::string &command)
    {
        yuan::buffer::ByteBuffer data;
        yuan::net::ssh::SshMessageCodec::write_string(data, command);
        return buffer_to_bytes(data);
    }

    std::vector<uint8_t> make_pty_request_data(uint32_t cols,
                                               uint32_t rows,
                                               uint32_t pixel_width,
                                               uint32_t pixel_height)
    {
        yuan::buffer::ByteBuffer data;
        yuan::net::ssh::SshMessageCodec::write_string(data, detect_terminal_type());
        yuan::net::ssh::SshMessageCodec::write_uint32(data, cols);
        yuan::net::ssh::SshMessageCodec::write_uint32(data, rows);
        yuan::net::ssh::SshMessageCodec::write_uint32(data, pixel_width);
        yuan::net::ssh::SshMessageCodec::write_uint32(data, pixel_height);
        const auto modes = make_terminal_modes();
        yuan::net::ssh::SshMessageCodec::write_string(data, std::string(
            reinterpret_cast<const char *>(modes.data()),
            modes.size()));
        return buffer_to_bytes(data);
    }

    std::vector<uint8_t> make_window_change_request_data(uint32_t cols,
                                                         uint32_t rows,
                                                         uint32_t pixel_width,
                                                         uint32_t pixel_height)
    {
        yuan::buffer::ByteBuffer data;
        yuan::net::ssh::SshMessageCodec::write_uint32(data, cols);
        yuan::net::ssh::SshMessageCodec::write_uint32(data, rows);
        yuan::net::ssh::SshMessageCodec::write_uint32(data, pixel_width);
        yuan::net::ssh::SshMessageCodec::write_uint32(data, pixel_height);
        return buffer_to_bytes(data);
    }

    std::vector<uint8_t> make_signal_request_data(const std::string &signal_name)
    {
        yuan::buffer::ByteBuffer data;
        yuan::net::ssh::SshMessageCodec::write_string(data, signal_name);
        return buffer_to_bytes(data);
    }

    yuan::buffer::ByteBuffer encode_channel_data_packet(uint32_t recipient_channel, const std::string &text)
    {
        yuan::net::ssh::SshChannelDataMessage msg;
        msg.recipient_channel = recipient_channel;
        msg.data.assign(text.begin(), text.end());
        return yuan::net::ssh::SshMessageCodec::encode_channel_data(msg);
    }
}
