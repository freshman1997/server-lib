#ifndef YUAN_RELEASE_SSH_CLI_MESSAGES_H
#define YUAN_RELEASE_SSH_CLI_MESSAGES_H

#include "buffer/byte_buffer.h"

#include <cstdint>
#include <string>
#include <vector>

namespace yuan::release_ssh::client
{
    std::vector<uint8_t> make_exec_request_data(const std::string &command);
    std::vector<uint8_t> make_pty_request_data(uint32_t cols,
                                               uint32_t rows,
                                               uint32_t pixel_width,
                                               uint32_t pixel_height);
    std::vector<uint8_t> make_window_change_request_data(uint32_t cols,
                                                         uint32_t rows,
                                                         uint32_t pixel_width,
                                                         uint32_t pixel_height);
    std::vector<uint8_t> make_signal_request_data(const std::string &signal_name);
    yuan::buffer::ByteBuffer encode_channel_data_packet(uint32_t recipient_channel, const std::string &text);
}

#endif
