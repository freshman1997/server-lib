#include "filesync_common.h"
#include "filesync_config.h"
#include "filesync_delta.h"
#include "filesync_protocol.h"
#include "filesync_scan.h"
#include "filesync_transfer.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace filesync::tests {
int failed = 0;

void check(bool value, const char* message) {
    if (!value) {
        ++failed;
        std::cerr << "FAIL: " << message << "\n";
    }
}

std::filesystem::path temporary_root() {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto path = std::filesystem::temp_directory_path() / ("filesync-delta-" + std::to_string(stamp));
    std::filesystem::create_directories(path);
    return path;
}

void write(const std::filesystem::path& path, const std::string& data) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out.write(data.data(), static_cast<std::streamsize>(data.size()));
}

std::string read(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

class Rebuilder final : public filesync::delta::OperationConsumer {
public:
    explicit Rebuilder(const std::filesystem::path& path) : input(path, std::ios::binary) {}
    void copy(std::uintmax_t offset, std::uint32_t size) override {
        input.seekg(static_cast<std::streamoff>(offset));
        std::string block(size, '\0');
        input.read(block.data(), static_cast<std::streamsize>(size));
        result += block;
    }
    void data(const char* bytes, std::size_t size) override { result.append(bytes, size); }
    std::string result;
private:
    std::ifstream input;
};

std::string rebuild(const std::filesystem::path& basis, const std::filesystem::path& source,
                    const filesync::delta::FileSignature& signature, bool& has_copy) {
    Rebuilder rebuilder(basis);
    class CopyObserver final : public filesync::delta::OperationConsumer {
    public:
        explicit CopyObserver(Rebuilder& value) : target(value) {}
        void copy(std::uintmax_t offset, std::uint32_t size) override { has_copy = true; target.copy(offset, size); }
        void data(const char* bytes, std::size_t size) override { target.data(bytes, size); }
        bool has_copy = false;
    private:
        Rebuilder& target;
    } observer(rebuilder);
    filesync::delta::stream_delta(source, signature, observer);
    has_copy = observer.has_copy;
    return rebuilder.result;
}

void test_delta_insert_delete_and_reuse() {
    const auto root = temporary_root();
    const auto basis = root / "basis.bin";
    const auto source = root / "source.bin";
    const std::string original = "0123456789abcdefghijABCDEFGHIJ9876543210";
    const std::string changed = "prefix-0123456789abcXdefghijABCDEFGHIJ-9876543210";
    write(basis, original);
    write(source, changed);
    const auto signature = filesync::delta::create_signature(basis, 10);
    bool has_copy = false;
    check(rebuild(basis, source, signature, has_copy) == changed, "rolling delta rebuilds insertion without loading whole file");
    check(has_copy, "delta reuses unchanged basis blocks");
    std::filesystem::remove_all(root);
}

void test_empty_and_digest() {
    const auto root = temporary_root();
    const auto empty = root / "empty";
    write(empty, {});
    const auto signature = filesync::delta::create_signature(empty, 4096);
    check(signature.blocks.empty() && signature.file_size == 0, "empty file has empty signature");
    check(signature.file_hash == filesync::sha256_file(empty), "file digest is stable SHA-256");
    std::filesystem::remove_all(root);
}

void test_path_validation() {
    check(filesync::is_safe_relative_path("a/b.txt"), "valid UTF-8 path shape accepted");
    check(!filesync::is_safe_relative_path("../escape"), "dotdot path rejected");
    check(!filesync::is_safe_relative_path("C:/escape"), "drive path rejected");
    check(!filesync::is_safe_relative_path("a\\b"), "backslash path rejected");
}

void test_recursive_exclude_patterns() {
    filesync::model::Config config;
    config.exclude_patterns = {"**/build*/**", "**/logs/**"};
    check(!filesync::scan::included(config, "repo/build/output.obj", false), "build directory is excluded");
    check(!filesync::scan::included(config, "repo/build-fast/output.obj", false), "build-prefixed directory is excluded");
    check(!filesync::scan::included(config, "repo/logs/server.log", false), "logs directory is excluded");
    check(filesync::scan::included(config, "repo/source/build.info", false), "build filename remains included");
    check(filesync::scan::included(config, "repo/source/BUILD.bazel", false), "BUILD filename remains included");
    check(filesync::scan::included(config, "repo/.git/objects/pack/data.pack", false), ".git remains included");
}

void test_strict_protocol() {
    filesync::model::Config config;
    const auto hash = filesync::sha256_bytes("", 0);
    const std::vector<std::string> valid{"HELLO filesync/3 change-me", "MANIFEST 1", "F empty 0 0 " + hash, "END"};
    check(filesync::protocol::parse_manifest(valid, config).size() == 1, "strict manifest accepts valid empty file");
    auto invalid = valid;
    invalid[1] = "MANIFEST 2";
    bool rejected = false;
    try { filesync::protocol::parse_manifest(invalid, config); } catch (const std::exception&) { rejected = true; }
    check(rejected, "strict manifest rejects count mismatch");
    invalid = valid;
    invalid.insert(invalid.end() - 1, valid[2]); invalid[1] = "MANIFEST 2";
    rejected = false;
    try { filesync::protocol::parse_manifest(invalid, config); } catch (const std::exception&) { rejected = true; }
    check(rejected, "strict manifest rejects duplicate paths");
    rejected = false;
    try { filesync::protocol::parse_need({"NEED 2", "GET x", "END"}); } catch (const std::exception&) { rejected = true; }
    check(rejected, "strict need rejects count mismatch");
    const auto root = temporary_root();
    write(root / "basis", "0123456789abcdef");
    std::vector<filesync::model::NeedRequest> requests{{"basis", filesync::delta::create_signature(root / "basis", 4)}};
    const auto encoded = filesync::protocol::encode_need(requests, {});
    std::istringstream stream(encoded); std::vector<std::string> lines; std::string line;
    while (std::getline(stream, line)) lines.push_back(line);
    const auto parsed = filesync::protocol::parse_need(lines);
    check(parsed.size() == 1 && parsed[0].basis->blocks.size() == 4, "need signatures round trip");
    std::filesystem::remove_all(root);
}

class CollectingWriter final : public filesync::transfer::LineWriter {
public:
    void write_line(const std::string& line) override { lines.push_back(line); }
    std::vector<std::string> lines;
};

void test_delete_request_uses_previous_state() {
    const auto root = temporary_root();
    const auto local_root = root / "local";
    write(local_root / "removed.txt", "old");
    filesync::model::Config config;
    config.sync_deletes = true;
    config.paths.push_back({local_root, "repo"});
    filesync::config::save_state(config, filesync::scan::paths(config));
    std::filesystem::remove(local_root / "removed.txt");

    CollectingWriter writer;
    filesync::transfer::send_need(config, {}, writer);
    check(writer.lines.size() == 3 && writer.lines[0] == "NEED_BEGIN 1" &&
        writer.lines[1] == "DELETE repo/removed.txt" && writer.lines[2] == "NEED_END",
        "delete sync emits paths missing from the remote manifest");
    std::filesystem::remove_all(root);
}

void test_delete_transfer_matches_receiver_count() {
    const auto root = temporary_root();
    const auto target_root = root / "target";
    write(target_root / "removed.txt", "old");
    filesync::model::Config config;
    config.sync_deletes = true;
    config.paths.push_back({target_root, "repo"});
    const std::vector<filesync::model::NeedRequest> requests{{"repo/removed.txt", {}, true}};
    filesync::model::Manifest manifest;
    filesync::transfer::FileSender sender(config, manifest, requests);
    std::vector<std::string> lines;
    std::string line;
    while (sender.next_line(line)) lines.push_back(line);
    check(lines.size() == 3 && lines[0] == "FILES 1" && lines[1] == "DELETE repo/removed.txt" && lines[2] == "END",
        "delete transfer counts delete records");

    filesync::transfer::Receiver receiver(config);
    bool complete = false;
    for (const auto& value : lines) complete = receiver.apply_line(value) || complete;
    check(complete && !std::filesystem::exists(target_root / "removed.txt"),
        "receiver applies delete transfer without count mismatch");
    std::filesystem::remove_all(root);
}

void test_local_delete_is_sent_to_remote() {
    const auto root = temporary_root();
    const auto local_root = root / "local";
    write(local_root / "removed.txt", "old");
    filesync::model::Config config;
    config.sync_deletes = true;
    config.paths.push_back({local_root, "repo"});
    const auto previous = filesync::scan::paths(config);
    filesync::config::save_state(config, previous);
    std::filesystem::remove(local_root / "removed.txt");

    filesync::model::Manifest remote = previous;
    CollectingWriter writer;
    filesync::transfer::send_need(config, remote, writer);
    check(writer.lines.size() == 3 && writer.lines[1] == "DELETE repo/removed.txt",
        "local deletion is sent to the remote peer");
    std::filesystem::remove_all(root);
}

void test_remote_change_wins_over_local_delete() {
    const auto root = temporary_root();
    const auto local_root = root / "local";
    write(local_root / "changed.txt", "old");
    filesync::model::Config config;
    config.sync_deletes = true;
    config.paths.push_back({local_root, "repo"});
    const auto previous = filesync::scan::paths(config);
    filesync::config::save_state(config, previous);
    std::filesystem::remove(local_root / "changed.txt");

    auto remote_state = previous.at("repo/changed.txt");
    ++remote_state.mtime;
    filesync::model::Manifest remote{{"repo/changed.txt", remote_state}};
    CollectingWriter writer;
    filesync::transfer::send_need(config, remote, writer);
    check(writer.lines.size() == 3 && writer.lines[1].rfind("GET ", 0) == 0,
        "remote change is not overwritten by a stale local deletion");
    std::filesystem::remove_all(root);
}

void test_incremental_file_sender_matches_receiver() {
    const auto root = temporary_root();
    const auto source_root = root / "source";
    const auto target_root = root / "target";
    write(source_root / "large.bin", std::string(20000, 'a') + std::string(20000, 'b'));

    filesync::model::Config source_config;
    source_config.chunk_size = 4096;
    source_config.paths.push_back({source_root, "work"});
    const auto manifest = filesync::scan::paths(source_config);
    const std::vector<filesync::model::NeedRequest> requests{{"work/large.bin", {}, false}};

    filesync::transfer::FileSender sender(source_config, manifest, requests);
    std::vector<std::string> incremental;
    std::string line;
    while (sender.next_line(line)) incremental.push_back(line);

    CollectingWriter writer;
    filesync::transfer::send_files(source_config, manifest, requests, writer);
    check(incremental == writer.lines, "incremental sender preserves full transfer protocol");
    check(incremental.size() > 4 && incremental.front() == "FILES 1" && incremental.back() == "END",
          "incremental sender emits bounded records through transfer end");

    filesync::model::Config target_config;
    target_config.paths.push_back({target_root, "work"});
    filesync::transfer::Receiver receiver(target_config);
    bool complete = false;
    for (const auto& value : incremental) complete = receiver.apply_line(value) || complete;
    check(complete, "receiver accepts incremental sender output");
    check(read(target_root / "large.bin") == read(source_root / "large.bin"),
          "incremental full transfer rebuilds file");
    std::filesystem::remove_all(root);
}
} // namespace filesync::tests

int main() {
    filesync::tests::test_delta_insert_delete_and_reuse();
    filesync::tests::test_empty_and_digest();
    filesync::tests::test_path_validation();
    filesync::tests::test_recursive_exclude_patterns();
    filesync::tests::test_strict_protocol();
    filesync::tests::test_delete_request_uses_previous_state();
    filesync::tests::test_delete_transfer_matches_receiver_count();
    filesync::tests::test_local_delete_is_sent_to_remote();
    filesync::tests::test_remote_change_wins_over_local_delete();
    filesync::tests::test_incremental_file_sender_matches_receiver();
    return filesync::tests::failed == 0 ? 0 : 1;
}
