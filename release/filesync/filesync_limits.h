#pragma once

#include <cstddef>

namespace filesync::limits {
constexpr std::size_t default_chunk_size = 32 * 1024;
constexpr std::size_t max_chunk_size = 32 * 1024;
constexpr std::size_t max_delta_data_size = 64 * 1024;
constexpr std::size_t max_control_buffer_size = 16 * 1024 * 1024;
constexpr std::size_t max_line_size = 2 * 1024 * 1024;
constexpr std::size_t max_manifest_entries = 1000000;
constexpr std::size_t max_transfer_entries = 1000000;
constexpr std::size_t max_signature_block_size = 32 * 1024;
constexpr std::size_t network_packet_size = 100 * 1024 * 1024;
constexpr std::size_t network_output_buffer_size = 64 * 1024 * 1024;
constexpr std::size_t digest_buffer_size = 64 * 1024;
constexpr std::size_t line_flush_size = 64 * 1024;
constexpr std::size_t transfer_batch_size = 1024 * 1024;
constexpr std::size_t max_batch_entries = 1000;
}
