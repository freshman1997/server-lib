#include "filesync_protocol.h"
#include "filesync_common.h"
#include "filesync_limits.h"
#include <charconv>
#include <limits>

namespace filesync::protocol
{
    std::string checked_path(const std::string &token);
    std::uint64_t number(const std::string &text)
    {
        std::uint64_t result = 0;
        const auto parsed = std::from_chars(text.data(), text.data() + text.size(), result);
        if (text.empty() || parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size())
            throw std::runtime_error("invalid unsigned number");
        return result;
    }
    bool valid_hash(const std::string &text)
    {
        if (text.size() != 64) return false;
        for (char ch : text)
            if (!(ch >= '0' && ch <= '9') && !(ch >= 'a' && ch <= 'f')) return false;
        return true;
    }
    std::string encode_manifest_entry(const std::string &path, const model::FileState &state)
    {
        std::ostringstream out;
        out << (state.is_directory ? "D " : "F ") << quote_token(path) << ' ' << state.size << ' ' << state.mtime << ' '
            << (state.is_directory ? "-" : (state.hash.empty() ? "-" : state.hash));
        return out.str();
    }
    model::FileState parse_manifest_entry(const std::string &line, std::string &path)
    {
        const auto parts = split_line(line);
        if (parts.size() != 5 || (parts[0] != "D" && parts[0] != "F")) throw std::runtime_error("invalid manifest entry");
        path = checked_path(parts[1]);
        model::FileState state{parts[0] == "D", number(parts[2]), number(parts[3]), parts[4]};
        if (state.is_directory ? (state.size != 0 || state.mtime != 0 || state.hash != "-") : (!state.hash.empty() && state.hash != "-" && !valid_hash(state.hash))) throw std::runtime_error("invalid manifest metadata");
        return state;
    }
    std::string encode_need_entry(const model::NeedRequest &request)
    {
        std::ostringstream out;
        out << (request.delete_path ? "DELETE " : "GET ") << quote_token(request.path);
        if (!request.delete_path && request.basis) {
            const auto &signature = *request.basis;
            out << ' ' << signature.block_size << ' ' << signature.file_size << ' ' << signature.file_hash << ' ' << signature.blocks.size();
            for (const auto &block : signature.blocks)
                out << "\nSIG " << block.index << ' ' << block.size << ' ' << block.weak << ' ' << block.strong;
        }
        return out.str();
    }
    std::string checked_path(const std::string &token)
    {
        const auto path = unquote_token(token);
        if (!is_safe_relative_path(path)) throw std::runtime_error("unsafe protocol path");
        return path;
    }
    model::Manifest parse_manifest(const std::vector<std::string> &lines, const model::Config &config)
    {
        if (lines.size() < 3 || lines.back() != "END") throw std::runtime_error("unterminated manifest");
        const auto hello = split_line(lines[0]);
        const auto header = split_line(lines[1]);
        if (hello.size() != 3 || hello[0] != "HELLO" || (hello[1] != "filesync/3" && hello[1] != "filesync/4") || unquote_token(hello[2]) != config.token)
            throw std::runtime_error("unauthorized peer");
        if (header.size() != 2 || header[0] != "MANIFEST" || number(header[1]) != lines.size() - 3)
            throw std::runtime_error("manifest count mismatch");
        model::Manifest result;
        for (std::size_t i = 2; i + 1 < lines.size(); ++i) {
            const auto parts = split_line(lines[i]);
            if (parts.size() != 5 || (parts[0] != "D" && parts[0] != "F")) throw std::runtime_error("invalid manifest entry");
            const auto path = checked_path(parts[1]);
            model::FileState state{parts[0] == "D", number(parts[2]), number(parts[3]), parts[4]};
            if (state.is_directory ? (state.size != 0 || state.mtime != 0 || state.hash != "-") : (state.hash != "-" && !valid_hash(state.hash)))
                throw std::runtime_error("invalid manifest metadata");
            if (!result.emplace(path, state).second) throw std::runtime_error("duplicate manifest path");
        }
        return result;
    }
    std::vector<model::NeedRequest> parse_need(const std::vector<std::string> &lines)
    {
        if (lines.size() < 2 || lines.back() != "END") throw std::runtime_error("unterminated need");
        const auto header = split_line(lines.front());
        if (header.size() != 2 || header[0] != "NEED") throw std::runtime_error("invalid need header");
        const auto declared = number(header[1]);
        if (declared > limits::max_transfer_entries) throw std::runtime_error("need is too large");
        std::set<std::string> seen;
        std::vector<model::NeedRequest> result;
        std::size_t entries = 0;
        for (std::size_t i = 1; i + 1 < lines.size(); ++i) {
            const auto parts = split_line(lines[i]);
            if (parts.size() < 2 || (parts[0] != "GET" && parts[0] != "DELETE")) throw std::runtime_error("invalid need entry");
            model::NeedRequest request{checked_path(parts[1]), {}, parts[0] == "DELETE"};
            if (!seen.insert(request.path).second) throw std::runtime_error("duplicate need path");
            ++entries;
            if (parts[0] == "DELETE") {
                if (parts.size() != 2) throw std::runtime_error("invalid delete");
                result.push_back(std::move(request));
                continue;
            }

            if (parts.size() != 2 && parts.size() != 6) {
                throw std::runtime_error("invalid get");
            }

            if (parts.size() == 6) {
                delta::FileSignature signature;
                const auto block = number(parts[2]);
                if (block == 0 || block > limits::max_signature_block_size || !valid_hash(parts[4])) {
                    throw std::runtime_error("invalid signature");
                }

                signature.block_size = static_cast<std::uint32_t>(block);
                signature.file_size = number(parts[3]);
                signature.file_hash = parts[4];
                const auto count = number(parts[5]);
                if (count != signature.file_size / block + (signature.file_size % block != 0) || count > limits::max_manifest_entries) {
                    throw std::runtime_error("signature count mismatch");
                }

                for (std::uint64_t n = 0; n < count; ++n) {
                    if (++i + 1 >= lines.size()) {
                        throw std::runtime_error("truncated signature");
                    }

                    const auto fields = split_line(lines[i]);
                    if (fields.size() != 5 || fields[0] != "SIG" || number(fields[1]) != n || !valid_hash(fields[4])) {
                        throw std::runtime_error("invalid block signature");
                    }

                    const auto size = number(fields[2]);
                    const auto weak = number(fields[3]);
                    const auto expected = std::min<std::uint64_t>(block, signature.file_size - n * block);
                    if (size != expected || weak > UINT32_MAX) {
                        throw std::runtime_error("invalid signature bounds");
                    }

                    signature.blocks.push_back({static_cast<std::uint32_t>(n), static_cast<std::uint32_t>(weak), fields[4], static_cast<std::uint32_t>(size)});
                }

                request.basis = std::move(signature);
            }

            result.push_back(std::move(request));
        }

        if (entries != declared) throw std::runtime_error("need count mismatch");
        return result;
    }
    std::string encode_need(const std::vector<model::NeedRequest> &requests, const std::vector<std::string> &deletes)
    {
        std::ostringstream out;
        out << "NEED " << requests.size() + deletes.size() << '\n';
        for (const auto &path : deletes) {
            out << "DELETE " << quote_token(path) << '\n';
        }
        
        for (const auto &request : requests) {
            out << "GET " << quote_token(request.path);
            if (request.basis) {
                const auto &signature = *request.basis;
                out << ' ' << signature.block_size << ' ' << signature.file_size << ' ' << signature.file_hash << ' ' << signature.blocks.size() << '\n';
                for (const auto &block : signature.blocks) {
                    out << "SIG " << block.index << ' ' << block.size << ' ' << block.weak << ' ' << block.strong << '\n';
                }
            } else {
                out << '\n';
            }
        }
        out << "END\n";
        return out.str();
    }

    std::vector<std::string> split_lines(const std::string &text)
    {
        std::istringstream input(text);
        std::vector<std::string> result;
        std::string line;
        while (std::getline(input, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            result.push_back(std::move(line));
        }
        return result;
    }

    bool take_end_frame(std::string &buffer, std::string &frame)
    {
        const auto pos = buffer.find("\nEND\n");
        if (pos == std::string::npos) return false;
        frame = buffer.substr(0, pos + 5);
        buffer.erase(0, pos + 5);
        return true;
    }

    std::string encode_manifest(const model::Config &config, const model::Manifest &manifest)
    {
        std::ostringstream out;
        out << "HELLO filesync/3 " << quote_token(config.token) << '\n';
        out << "MANIFEST " << manifest.size() << '\n';
        for (const auto &entry : manifest) {
            const auto &path = entry.first;
            const auto &state = entry.second;
            out << encode_manifest_entry(path, state) << '\n';
        }
        out << "END\n";
        return out.str();
    }
}
