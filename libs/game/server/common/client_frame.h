#ifndef YUAN_GAME_SERVER_COMMON_CLIENT_FRAME_H
#define YUAN_GAME_SERVER_COMMON_CLIENT_FRAME_H

#include "common/codec/game_binary_codec.h"

#include <optional>
#include <mutex>
#include <string>
#include <unordered_map>

namespace yuan::game::server
{
    struct ClientFrameHeader
    {
        PlayerUid player_uid = 0;
        RoleId role_id = 0;
        std::uint64_t sequence = 0;
        std::uint32_t route_service = 0;
        std::uint32_t route_method = 0;
    };

    struct ClientFrame
    {
        ClientFrameHeader header;
        yuan::rpc::Bytes payload;
    };

    struct ClientFrameValidationOptions
    {
        std::size_t max_frame_bytes = 64 * 1024;
        bool require_strict_sequence = true;
        std::uint32_t max_frames_per_window = 0;
        std::uint64_t rate_window_ms = 1000;
    };

    struct ClientFrameValidationResult
    {
        bool ok = false;
        std::string error;
    };

    enum class ClientFrameStreamStatus
    {
        frame,
        need_more,
        protocol_error
    };

    struct ClientFrameStreamResult
    {
        ClientFrameStreamStatus status = ClientFrameStreamStatus::need_more;
        std::optional<ClientFrame> frame;
        std::string error;
    };

    class ClientFrameReplayGuard
    {
    public:
        [[nodiscard]] ClientFrameValidationResult validate(const ClientFrame &frame, const ClientFrameValidationOptions &options = {});
        void erase_session(std::uint64_t gateway_session_id);

    private:
        struct RateState
        {
            std::uint64_t window_start_ms = 0;
            std::uint32_t count = 0;
        };

        mutable std::mutex mutex_;
        std::unordered_map<std::uint64_t, std::uint64_t> last_sequence_by_session_;
        std::unordered_map<std::uint64_t, RateState> rate_by_session_;
    };

    bool encode_client_frame(const ClientFrame &frame, yuan::rpc::Bytes &out);
    [[nodiscard]] std::optional<ClientFrame> decode_client_frame(const yuan::rpc::Bytes &in);
    [[nodiscard]] yuan::rpc::Bytes encode_framed_client_payload(ClientFrameHeader header, yuan::rpc::Bytes payload);

    class ClientFrameStreamDecoder
    {
    public:
        explicit ClientFrameStreamDecoder(ClientFrameValidationOptions options = {});

        void append(const std::uint8_t *data, std::size_t size);
        void append(const yuan::rpc::Bytes &bytes);
        [[nodiscard]] ClientFrameStreamResult next();
        [[nodiscard]] std::size_t buffered_size() const;
        void clear();

    private:
        ClientFrameValidationOptions options_;
        yuan::rpc::Bytes buffer_;
    };
}

#endif
