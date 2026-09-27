#include "filesync_transfer.h"
#include "filesync_limits.h"
#include "filesync_config.h"
#include "filesync_common.h"
#include "filesync_delta.h"
#include "filesync_protocol.h"
#include "filesync_scan.h"

#include <algorithm>
#include <atomic>
#include <limits>
#include <chrono>
#include <system_error>

namespace filesync::transfer {

std::atomic<std::uint64_t> temporary_file_sequence{0};

void send_need(const model::Config& config, const model::Manifest& remote, LineWriter& writer) {
    const auto local = scan::paths(config);
    const auto previous = config::load_state(config);
    std::vector<model::NeedRequest> requests;
    std::vector<std::string> deletes;
    std::size_t total = 0;
    for (const auto& entry : remote) {
        if (!scan::included(config, entry.first, entry.second.is_directory)) continue;
        const auto it = local.find(entry.first);
        const auto old = previous.find(entry.first);
        const bool remote_unchanged = old != previous.end() &&
            old->second.is_directory == entry.second.is_directory &&
            old->second.size == entry.second.size && old->second.mtime == entry.second.mtime;
        if (config.sync_deletes && it == local.end() && remote_unchanged) {
            deletes.push_back(entry.first);
            continue;
        }
        const bool needs = it == local.end() || it->second.is_directory != entry.second.is_directory ||
            (!entry.second.is_directory &&
                (it->second.size != entry.second.size || it->second.mtime != entry.second.mtime));
        if (needs) requests.push_back({entry.first, {}, false});
    }
    if (config.sync_deletes) {
        for (const auto& entry : previous) {
            if (remote.find(entry.first) != remote.end()) continue;
            if (scan::included(config, entry.first, entry.second.is_directory)) {
                deletes.push_back(entry.first);
            }
        }
    }
    total = requests.size() + deletes.size();
    writer.write_line("NEED_BEGIN " + std::to_string(total));
    std::size_t emitted = 0;
    for (const auto& request_value : requests) {
        const auto& entry = *remote.find(request_value.path);
        const auto it = local.find(entry.first);
        model::NeedRequest request{entry.first, {}, false};
        if (it != local.end() && !it->second.is_directory && !entry.second.is_directory) {
            request.basis = delta::create_signature(scan::local_path(config, entry.first),
                                                    static_cast<std::uint32_t>(config.chunk_size));
        }
        writer.write_line(protocol::encode_need_entry(request));
        if (++emitted % protocol::max_batch_entries == 0) writer.flush();
    }
    for (const auto& path : deletes) {
        writer.write_line(protocol::encode_need_entry({path, {}, true}));
        if (++emitted % protocol::max_batch_entries == 0) writer.flush();
    }
    writer.write_line("NEED_END"); writer.flush();
}

void send_files(const model::Config& config, const model::Manifest& manifest,
                const std::vector<model::NeedRequest>& requests, LineWriter& writer) {
    FileSender sender(config, manifest, requests);
    std::string line;
    while (sender.next_line(line)) writer.write_line(line);
    std::cout << "filesync send complete full=" << sender.full_count()
              << " delta=" << sender.delta_count() << std::endl;
}

class FileSender::Impl {
public:
    Impl(const model::Config& config_value, const model::Manifest& manifest_value,
         const std::vector<model::NeedRequest>& requests_value)
        : config(config_value), manifest(manifest_value), requests(requests_value), bytes(config.chunk_size) {
        transfer_count = requests.size();
    }

    bool next_line(std::string& line) {
        if (!header_sent) {
            header_sent = true;
            line = "FILES " + std::to_string(transfer_count);
            return true;
        }
        if (phase == Phase::FullData) return next_full(line);
        if (phase == Phase::DeltaData) return next_delta(line);
        if (phase == Phase::FullEnd) { phase = Phase::Next; line = "PUT_END"; return true; }
        if (phase == Phase::DeltaEnd) { phase = Phase::Next; line = "DELTA_END"; return true; }

        while (request_index < requests.size()) {
            const auto& request = requests[request_index++];
        if (request.delete_path) {
                line = "DELETE " + quote_token(request.path);
                return true;
        }
        const auto it = manifest.find(request.path);
        if (it == manifest.end()) throw std::runtime_error("requested path is absent from manifest");
        const auto& state = it->second;
        if (state.is_directory) {
                line = "DIR " + quote_token(request.path);
                return true;
        }
        // Prefer the path captured by the scan. It preserves the platform's
        // exact filename representation; resolve from the remote name only
        // when the captured entry is unavailable.
        auto source = state.local_path;
        if (source.empty() || !std::filesystem::is_regular_file(source)) source = scan::local_path(config, request.path);
        if (!std::filesystem::is_regular_file(source)) {
            throw std::runtime_error("source file disappeared: " + source.string());
        }
        model::FileState transfer_state = state;
        transfer_state.size = std::filesystem::file_size(source);
        transfer_state.mtime = file_time_to_seconds(std::filesystem::last_write_time(source));
        transfer_state.hash = filesync::sha256_file(source);
        if (std::filesystem::file_size(source) != transfer_state.size ||
            file_time_to_seconds(std::filesystem::last_write_time(source)) != transfer_state.mtime) {
            throw std::runtime_error("source file changed before transfer: " + source.string());
        }
        if (request.basis) {
                ++delta_count_value;
                delta_stream = std::make_unique<delta::OperationStream>(source, *request.basis);
                phase = Phase::DeltaData;
                line = "DELTA_BEGIN " + quote_token(request.path) + ' ' + std::to_string(transfer_state.size) + ' ' +
                    std::to_string(transfer_state.mtime) + ' ' + transfer_state.hash + ' ' + request.basis->file_hash;
                return true;
        } else {
                ++full_count_value;
                input.open(source, std::ios::binary);
            if (!input) throw std::runtime_error("failed to open source file");
                phase = Phase::FullData;
                line = "PUT_BEGIN " + quote_token(request.path) + ' ' + std::to_string(transfer_state.size) + ' ' +
                    std::to_string(transfer_state.mtime) + ' ' + transfer_state.hash;
                return true;
            }
        }
        if (!end_sent) { end_sent = true; line = "END"; return true; }
        return false;
    }

    bool next_full(std::string& line) {
        input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        const auto count = input.gcount();
        if (count > 0) {
            line = "CHUNK " + hex_encode(std::vector<char>(bytes.begin(), bytes.begin() + count));
            return true;
        }
        input.close();
        phase = Phase::FullEnd;
        return next_line(line);
    }

    bool next_delta(std::string& line) {
        delta::Operation operation;
        if (delta_stream->next(operation)) {
            if (operation.kind == delta::Operation::Kind::Copy) {
                line = "COPY " + std::to_string(operation.offset) + " " + std::to_string(operation.size);
            } else {
                line = "DATA " + hex_encode(operation.data);
            }
            return true;
        }
        delta_stream.reset();
        phase = Phase::DeltaEnd;
        return next_line(line);
    }

    enum class Phase { Next, FullData, FullEnd, DeltaData, DeltaEnd };
    const model::Config& config;
    const model::Manifest& manifest;
    const std::vector<model::NeedRequest>& requests;
    std::size_t transfer_count = 0;
    std::size_t request_index = 0;
    std::size_t full_count_value = 0;
    std::size_t delta_count_value = 0;
    bool header_sent = false;
    bool end_sent = false;
    Phase phase = Phase::Next;
    std::ifstream input;
    std::vector<char> bytes;
    std::unique_ptr<delta::OperationStream> delta_stream;
};

FileSender::FileSender(const model::Config& config, const model::Manifest& manifest,
                       const std::vector<model::NeedRequest>& requests)
    : impl_(std::make_unique<Impl>(config, manifest, requests)) {}
FileSender::~FileSender() = default;
FileSender::FileSender(FileSender&&) noexcept = default;
FileSender& FileSender::operator=(FileSender&&) noexcept = default;
bool FileSender::next_line(std::string& line) { return impl_->next_line(line); }
std::size_t FileSender::full_count() const { return impl_->full_count_value; }
std::size_t FileSender::delta_count() const { return impl_->delta_count_value; }

Receiver::Receiver(const model::Config& config) : config_(config) {}
Receiver::~Receiver() { abort_file(); }
std::size_t Receiver::completed() const { return completed_; }
void Receiver::clear_file() {
    kind_ = Kind::None;
    path_.clear();
    output_.close();
    received_ = 0;
    expected_ = model::FileState{};
    target_.clear();
    basis_.clear();
    temp_.clear();
}

void Receiver::abort_file() {
    output_.close();
    if (!temp_.empty()) {
        std::error_code ec;
        std::filesystem::remove(temp_, ec);
    }
    clear_file();
}

void Receiver::reset() {
    clear_file();
    completed_ = 0;
    expected_entries_ = 0;
    batch_started_ = false;
}

void Receiver::begin(const std::vector<std::string>& parts, bool is_delta) {
    if ((!is_delta && parts.size() != 5) || (is_delta && parts.size() != 6)) throw std::runtime_error("invalid transfer header");
    path_ = unquote_token(parts[1]);
    if (!is_safe_relative_path(path_) || !scan::included(config_, path_, false)) throw std::runtime_error("unsafe transfer path");
    expected_ = {false, protocol::number(parts[2]), protocol::number(parts[3]), parts[4]};
    if (!protocol::valid_hash(expected_.hash)) throw std::runtime_error("invalid target hash");
    target_ = scan::local_path(config_, path_);
    std::filesystem::create_directories(target_.parent_path());
    temp_ = target_;
    temp_ += ".filesync.tmp." + std::to_string(++temporary_file_sequence);
    basis_ = target_;
    if (is_delta) {
        if (!protocol::valid_hash(parts[5]) || !std::filesystem::is_regular_file(basis_) ||
            filesync::sha256_file(basis_) != parts[5]) throw std::runtime_error("delta basis mismatch");
        kind_ = Kind::Delta;
    } else kind_ = Kind::Full;
    output_.open(temp_, std::ios::binary | std::ios::trunc);
    if (!output_) throw std::runtime_error("cannot open transfer temp");
    received_ = 0;
}

void Receiver::finish() {
    output_.close();
    if (!output_) {
        abort_file();
        throw std::runtime_error("failed to flush received file");
    }
    if (received_ != expected_.size || !std::filesystem::exists(temp_) || filesync::sha256_file(temp_) != expected_.hash) {
        const auto failed_path = path_;
        const auto failed_size = expected_.size;
        const auto failed_received = received_;
        std::error_code ec; std::filesystem::remove(temp_, ec); clear_file();
        throw std::runtime_error("received file size/hash mismatch: " + failed_path +
            " expected=" + std::to_string(failed_size) + " received=" + std::to_string(failed_received));
    }
    std::error_code ec;
    const auto backup = target_.string() + ".filesync.backup." + std::to_string(++temporary_file_sequence);
    const bool had_target = std::filesystem::exists(target_, ec);
    if (had_target) {
        if (config_.conflict_strategy == "keep_both" && std::filesystem::is_regular_file(target_, ec) &&
            filesync::sha256_file(target_) != expected_.hash) {
            const auto conflict = target_.string() + ".conflict." + std::to_string(expected_.mtime);
            std::filesystem::rename(target_, conflict, ec);
        } else {
            std::filesystem::rename(target_, backup, ec);
        }
        if (ec) { abort_file(); throw std::runtime_error("failed to preserve target"); }
    }
    std::filesystem::rename(temp_, target_, ec);
    if (ec && had_target) {
        std::error_code restore_ec;
        std::filesystem::rename(backup, target_, restore_ec);
    }
    if (!ec && had_target && std::filesystem::exists(backup)) std::filesystem::remove_all(backup, ec);
    if (ec) { std::filesystem::remove(temp_, ec); clear_file(); throw std::runtime_error("atomic replace failed"); }
    if (expected_.mtime != 0) {
        const auto desired = std::filesystem::file_time_type::clock::from_sys(std::chrono::system_clock::time_point(std::chrono::nanoseconds(expected_.mtime)));
        std::filesystem::last_write_time(target_, desired, ec);
        if (ec) {
            clear_file();
            throw std::runtime_error("failed to restore file modification time");
        }
    }
    ++completed_;
    clear_file();
}

bool Receiver::apply_line(const std::string& line) {
    if (line == "END") {
        if (!batch_started_ || kind_ != Kind::None || completed_ != expected_entries_)
            throw std::runtime_error("transfer count mismatch");
        expected_entries_ = 0;
        batch_started_ = false;
        return true;
    }
    const auto parts = split_line(line);
    if (parts.empty()) return false;
    if (parts[0] == "FILES") {
        if (parts.size() != 2 || batch_started_) throw std::runtime_error("invalid files count");
        expected_entries_ = static_cast<std::size_t>(protocol::number(parts[1]));
        if (expected_entries_ > limits::max_transfer_entries) throw std::runtime_error("too many transfer entries");
        completed_ = 0;
        batch_started_ = true;
        return false;
    }
    if (kind_ == Kind::None && parts[0] == "DIR") {
        if (parts.size() != 2) throw std::runtime_error("invalid directory");
        const auto path = unquote_token(parts[1]);
        if (!is_safe_relative_path(path) || !scan::included(config_, path, true)) throw std::runtime_error("unsafe directory");
        const auto target = scan::local_path(config_, path);
        std::error_code ec;
        if (std::filesystem::is_regular_file(target, ec)) std::filesystem::remove(target, ec);
        std::filesystem::create_directories(target, ec);
        if (ec) throw std::runtime_error("failed to create directory");
        ++completed_;
        return false;
    }
    if (kind_ == Kind::None && parts[0] == "DELETE") {
        if (parts.size() != 2) throw std::runtime_error("invalid delete");
        const auto path = unquote_token(parts[1]);
        if (!is_safe_relative_path(path) || !scan::included(config_, path, false)) throw std::runtime_error("unsafe delete");
        const auto target = scan::local_path(config_, path);
        std::error_code ec;
        std::filesystem::remove_all(target, ec);
        if (ec) throw std::runtime_error("failed to apply delete");
        ++completed_;
        return false;
    }
    if (kind_ == Kind::None && parts[0] == "PUT_BEGIN") { begin(parts, false); return false; }
    if (kind_ == Kind::None && parts[0] == "DELTA_BEGIN") { begin(parts, true); return false; }
    if (kind_ == Kind::Full && parts[0] == "CHUNK") {
        if (parts.size() != 2) throw std::runtime_error("invalid chunk");
        const auto bytes = hex_decode(parts[1]); if (received_ + bytes.size() > expected_.size) throw std::runtime_error("full transfer overflow");
        output_.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        if (!output_) throw std::runtime_error("failed to write full transfer");
        received_ += bytes.size(); return false;
    }
    if (kind_ == Kind::Full && parts[0] == "PUT_END") { finish(); return false; }
    if (kind_ == Kind::Delta && parts[0] == "COPY") {
        if (parts.size() != 3) throw std::runtime_error("invalid copy");
        const auto offset = protocol::number(parts[1]); const auto size = protocol::number(parts[2]);
        if (offset > std::filesystem::file_size(basis_) || size > std::filesystem::file_size(basis_) - offset || received_ + size > expected_.size) throw std::runtime_error("copy bounds");
        std::ifstream input(basis_, std::ios::binary); input.seekg(static_cast<std::streamoff>(offset)); std::vector<char> bytes(size); input.read(bytes.data(), static_cast<std::streamsize>(size));
        if (input.gcount() != static_cast<std::streamsize>(size)) throw std::runtime_error("short basis copy");
        output_.write(bytes.data(), static_cast<std::streamsize>(size));
        if (!output_) throw std::runtime_error("failed to write delta copy");
        received_ += size; return false;
    }
    if (kind_ == Kind::Delta && parts[0] == "DATA") {
        if (parts.size() != 2) throw std::runtime_error("invalid data"); const auto bytes = hex_decode(parts[1]);
        if (bytes.size() > limits::max_delta_data_size || received_ + bytes.size() > expected_.size) throw std::runtime_error("data overflow");
        output_.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        if (!output_) throw std::runtime_error("failed to write delta data");
        received_ += bytes.size(); return false;
    }
    if (kind_ == Kind::Delta && parts[0] == "DELTA_END") { finish(); return false; }
    throw std::runtime_error("unexpected transfer record");
}
}
