#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>
#include "filesync_delta.h"
#include "filesync_limits.h"

namespace filesync::model {

struct SyncPath {
    std::filesystem::path local;
    std::string remote_prefix;
};

struct Config {
    std::string listen_host = "0.0.0.0";
    std::uint16_t listen_port = 9095;
    std::string peer_host;
    std::uint16_t peer_port = 9095;
    std::string token = "change-me";
    std::string conflict_strategy = "keep_both";
    std::string sync_mode = "bidirectional";
    bool sync_deletes = false;
    int scan_interval_ms = 60000;
    std::size_t chunk_size = limits::default_chunk_size;
    std::vector<std::string> include_extensions;
    std::vector<std::string> include_patterns;
    std::vector<std::string> exclude_patterns;
    std::vector<SyncPath> paths;
};

struct FileState {
    bool is_directory = false;
    std::uintmax_t size = 0;
    std::uint64_t mtime = 0;
    std::string hash;
    std::filesystem::path local_path;
};

using Manifest = std::map<std::string, FileState>;

struct NeedRequest {
    std::string path;
    std::optional<delta::FileSignature> basis;
    bool delete_path = false;
};

} // namespace filesync::model
