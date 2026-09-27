#pragma once
#include "filesync_model.h"
#include "filesync_limits.h"
#include <set>

namespace filesync::protocol {
constexpr std::size_t max_batch_entries = limits::max_batch_entries;
constexpr std::size_t max_control_buffer = limits::max_control_buffer_size;
constexpr std::size_t max_line_size = limits::max_line_size;
std::uint64_t number(const std::string& text);
bool valid_hash(const std::string& text);
model::Manifest parse_manifest(const std::vector<std::string>& lines, const model::Config& config);
std::vector<model::NeedRequest> parse_need(const std::vector<std::string>& lines);
std::string encode_need(const std::vector<model::NeedRequest>& requests, const std::vector<std::string>& deletes);
std::vector<std::string> split_lines(const std::string& text);
bool take_end_frame(std::string& buffer, std::string& frame);
std::string encode_manifest(const model::Config& config, const model::Manifest& manifest);
std::string encode_manifest_entry(const std::string& path, const model::FileState& state);
std::string encode_need_entry(const model::NeedRequest& request);
model::FileState parse_manifest_entry(const std::string& line, std::string& path);
}
