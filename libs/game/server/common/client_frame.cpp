#include "common/client_frame.h"

#include "base/time.h"

#include <limits>

namespace yuan::game::server
{
    namespace
    {
        constexpr std::uint32_t client_frame_magic = 0x43534631; // CSF1
        constexpr std::uint32_t client_frame_version = 1;
        constexpr std::size_t client_frame_header_size = sizeof(std::uint32_t) * 5 + sizeof(std::uint64_t) * 5;

        void append_u32(yuan::rpc::Bytes &out, std::uint32_t value)
        {
            out.push_back(static_cast<std::uint8_t>((value >> 24) & 0xff));
            out.push_back(static_cast<std::uint8_t>((value >> 16) & 0xff));
            out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xff));
            out.push_back(static_cast<std::uint8_t>(value & 0xff));
        }

        void append_u64(yuan::rpc::Bytes &out, std::uint64_t value)
        {
            out.push_back(static_cast<std::uint8_t>((value >> 56) & 0xff));
            out.push_back(static_cast<std::uint8_t>((value >> 48) & 0xff));
            out.push_back(static_cast<std::uint8_t>((value >> 40) & 0xff));
            out.push_back(static_cast<std::uint8_t>((value >> 32) & 0xff));
            out.push_back(static_cast<std::uint8_t>((value >> 24) & 0xff));
            out.push_back(static_cast<std::uint8_t>((value >> 16) & 0xff));
            out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xff));
            out.push_back(static_cast<std::uint8_t>(value & 0xff));
        }

        bool read_u32(const yuan::rpc::Bytes &in, std::size_t &offset, std::uint32_t &value)
        {
            if (in.size() - offset < sizeof(std::uint32_t)) {
                return false;
            }
            value = (static_cast<std::uint32_t>(in[offset]) << 24) |
                    (static_cast<std::uint32_t>(in[offset + 1]) << 16) |
                    (static_cast<std::uint32_t>(in[offset + 2]) << 8) |
                    static_cast<std::uint32_t>(in[offset + 3]);
            offset += sizeof(std::uint32_t);
            return true;
        }

        bool read_u64(const yuan::rpc::Bytes &in, std::size_t &offset, std::uint64_t &value)
        {
            if (in.size() - offset < sizeof(std::uint64_t)) {
                return false;
            }
            value = (static_cast<std::uint64_t>(in[offset]) << 56) |
                    (static_cast<std::uint64_t>(in[offset + 1]) << 48) |
                    (static_cast<std::uint64_t>(in[offset + 2]) << 40) |
                    (static_cast<std::uint64_t>(in[offset + 3]) << 32) |
                    (static_cast<std::uint64_t>(in[offset + 4]) << 24) |
                    (static_cast<std::uint64_t>(in[offset + 5]) << 16) |
                    (static_cast<std::uint64_t>(in[offset + 6]) << 8) |
                    static_cast<std::uint64_t>(in[offset + 7]);
            offset += sizeof(std::uint64_t);
            return true;
        }
    }

    bool encode_client_frame(const ClientFrame &frame, yuan::rpc::Bytes &out)
    {
        if (frame.payload.size() > std::numeric_limits<std::uint32_t>::max()) {
            return false;
        }
        out.clear();
        append_u32(out, client_frame_magic);
        append_u32(out, client_frame_version);
        append_u64(out, frame.header.player_uid);
        append_u64(out, frame.header.role_id);
        append_u64(out, frame.header.zone_service_id);
        append_u64(out, frame.header.gateway_session_id);
        append_u64(out, frame.header.sequence);
        append_u32(out, frame.header.route_service);
        append_u32(out, frame.header.route_method);
        append_u32(out, static_cast<std::uint32_t>(frame.payload.size()));
        out.insert(out.end(), frame.payload.begin(), frame.payload.end());
        return true;
    }

    std::optional<ClientFrame> decode_client_frame(const yuan::rpc::Bytes &in)
    {
        ClientFrame frame;
        std::size_t offset = 0;
        std::uint32_t magic = 0;
        std::uint32_t version = 0;
        std::uint32_t payload_size = 0;
        if (!read_u32(in, offset, magic) || magic != client_frame_magic ||
            !read_u32(in, offset, version) || version != client_frame_version ||
            !read_u64(in, offset, frame.header.player_uid) ||
            !read_u64(in, offset, frame.header.role_id) ||
            !read_u64(in, offset, frame.header.zone_service_id) ||
            !read_u64(in, offset, frame.header.gateway_session_id) ||
            !read_u64(in, offset, frame.header.sequence) ||
            !read_u32(in, offset, frame.header.route_service) ||
            !read_u32(in, offset, frame.header.route_method) ||
            !read_u32(in, offset, payload_size) || in.size() - offset != payload_size) {
            return std::nullopt;
        }
        frame.payload.assign(in.begin() + static_cast<std::ptrdiff_t>(offset), in.end());
        return frame;
    }

    yuan::rpc::Bytes encode_framed_client_payload(ClientFrameHeader header, yuan::rpc::Bytes payload)
    {
        yuan::rpc::Bytes out;
        (void)encode_client_frame(ClientFrame{std::move(header), std::move(payload)}, out);
        return out;
    }

    ClientFrameStreamDecoder::ClientFrameStreamDecoder(ClientFrameValidationOptions options)
        : options_(std::move(options))
    {
    }

    void ClientFrameStreamDecoder::append(const std::uint8_t *data, std::size_t size)
    {
        if (size == 0) {
            return;
        }
        buffer_.insert(buffer_.end(), data, data + size);
    }

    void ClientFrameStreamDecoder::append(const yuan::rpc::Bytes &bytes)
    {
        append(bytes.data(), bytes.size());
    }

    ClientFrameStreamResult ClientFrameStreamDecoder::next()
    {
        if (buffer_.size() < sizeof(std::uint32_t)) {
            return {ClientFrameStreamStatus::need_more, std::nullopt, {}};
        }

        std::size_t offset = 0;
        std::uint32_t magic = 0;
        if (!read_u32(buffer_, offset, magic) || magic != client_frame_magic) {
            return {ClientFrameStreamStatus::protocol_error, std::nullopt, "bad client frame magic"};
        }

        if (buffer_.size() < client_frame_header_size) {
            return {ClientFrameStreamStatus::need_more, std::nullopt, {}};
        }

        std::uint32_t version = 0;
        ClientFrameHeader header;
        std::uint32_t payload_size = 0;
        if (!read_u32(buffer_, offset, version) || version != client_frame_version ||
            !read_u64(buffer_, offset, header.player_uid) ||
            !read_u64(buffer_, offset, header.role_id) ||
            !read_u64(buffer_, offset, header.zone_service_id) ||
            !read_u64(buffer_, offset, header.gateway_session_id) ||
            !read_u64(buffer_, offset, header.sequence) ||
            !read_u32(buffer_, offset, header.route_service) ||
            !read_u32(buffer_, offset, header.route_method) ||
            !read_u32(buffer_, offset, payload_size)) {
            return {ClientFrameStreamStatus::protocol_error, std::nullopt, "malformed client frame header"};
        }
        if (payload_size > options_.max_frame_bytes) {
            return {ClientFrameStreamStatus::protocol_error, std::nullopt, "client frame payload too large"};
        }

        const auto total_size = client_frame_header_size + static_cast<std::size_t>(payload_size);
        if (buffer_.size() < total_size) {
            return {ClientFrameStreamStatus::need_more, std::nullopt, {}};
        }

        yuan::rpc::Bytes frame_bytes(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(total_size));
        auto frame = decode_client_frame(frame_bytes);
        if (!frame) {
            return {ClientFrameStreamStatus::protocol_error, std::nullopt, "malformed client frame"};
        }
        buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(total_size));
        return {ClientFrameStreamStatus::frame, std::move(frame), {}};
    }

    std::size_t ClientFrameStreamDecoder::buffered_size() const
    {
        return buffer_.size();
    }

    void ClientFrameStreamDecoder::clear()
    {
        buffer_.clear();
    }

    ClientFrameValidationResult ClientFrameReplayGuard::validate(const ClientFrame &frame, const ClientFrameValidationOptions &options)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (frame.header.gateway_session_id == 0) {
            return {false, "missing gateway session id"};
        }
        if (frame.header.sequence == 0) {
            return {false, "missing client frame sequence"};
        }
        if (frame.payload.size() > options.max_frame_bytes) {
            return {false, "client frame payload too large"};
        }
        if (options.max_frames_per_window != 0) {
            auto &rate = rate_by_session_[frame.header.gateway_session_id];
            const auto now_ms = yuan::base::time::steady_now_ms();
            const auto window_ms = options.rate_window_ms == 0 ? 1000 : options.rate_window_ms;
            if (rate.window_start_ms == 0 || rate.window_start_ms + window_ms <= now_ms) {
                rate.window_start_ms = now_ms;
                rate.count = 0;
            }
            if (rate.count >= options.max_frames_per_window) {
                return {false, "client frame rate limited"};
            }
            ++rate.count;
        }
        const auto last = last_sequence_by_session_.find(frame.header.gateway_session_id);
        if (last != last_sequence_by_session_.end()) {
            const auto expected = last->second + 1;
            if ((options.require_strict_sequence && frame.header.sequence != expected) ||
                (!options.require_strict_sequence && frame.header.sequence <= last->second)) {
                return {false, "client frame sequence replay or gap"};
            }
        }
        last_sequence_by_session_[frame.header.gateway_session_id] = frame.header.sequence;
        return {true, {}};
    }

    void ClientFrameReplayGuard::erase_session(std::uint64_t gateway_session_id)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        last_sequence_by_session_.erase(gateway_session_id);
        rate_by_session_.erase(gateway_session_id);
    }
}
