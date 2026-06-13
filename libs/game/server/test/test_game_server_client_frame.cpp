#include "common/client_frame.h"

#include "base/time.h"

#include <cstdlib>
#include <iostream>
#include <random>

namespace
{
    bool require(bool condition, const char *message)
    {
        if (!condition) {
            std::cerr << message << '\n';
            return false;
        }
        return true;
    }
}

int main()
{
    using namespace yuan::game::server;

    const auto route = game_route::gateway_game_forward();
    const auto payload = yuan::rpc::Codec<std::string>::encode("move:1,2");
    const ClientFrame frame{ClientFrameHeader{90001, 10001, 4504699675869185ULL, 77, 1, route.service, route.method}, payload};

    yuan::rpc::Bytes encoded;
    if (!require(encode_client_frame(frame, encoded), "client frame should encode")) {
        return 1;
    }
    const auto decoded = decode_client_frame(encoded);
    if (!require(decoded.has_value(), "client frame should decode")) {
        return 2;
    }
    if (!require(decoded->header.player_uid == frame.header.player_uid && decoded->header.role_id == frame.header.role_id &&
                     decoded->header.zone_service_id == frame.header.zone_service_id && decoded->header.gateway_session_id == frame.header.gateway_session_id,
                 "client frame header should roundtrip")) {
        return 3;
    }
    if (!require(decoded->payload == payload, "client frame payload should roundtrip")) {
        return 4;
    }
    ClientFrameReplayGuard replay_guard;
    if (!require(replay_guard.validate(*decoded).ok, "first client frame sequence should pass")) {
        return 5;
    }
    if (!require(!replay_guard.validate(*decoded).ok, "duplicate client frame sequence should be rejected")) {
        return 6;
    }
    auto next = *decoded;
    next.header.sequence = 2;
    if (!require(replay_guard.validate(next).ok, "next client frame sequence should pass")) {
        return 7;
    }
    auto too_large = next;
    too_large.header.sequence = 3;
    too_large.payload.resize(8);
    if (!require(!replay_guard.validate(too_large, ClientFrameValidationOptions{4, true}).ok, "oversized client frame payload should be rejected")) {
        return 8;
    }
    yuan::base::time::set_steady_time_for_test(1000);
    ClientFrameReplayGuard rate_guard;
    ClientFrameValidationOptions rate_options;
    rate_options.max_frames_per_window = 2;
    rate_options.rate_window_ms = 1000;
    auto rate_frame = *decoded;
    rate_frame.header.gateway_session_id = 88;
    rate_frame.header.sequence = 1;
    if (!require(rate_guard.validate(rate_frame, rate_options).ok, "first frame in rate window should pass")) {
        return 10;
    }
    rate_frame.header.sequence = 2;
    if (!require(rate_guard.validate(rate_frame, rate_options).ok, "second frame in rate window should pass")) {
        return 11;
    }
    rate_frame.header.sequence = 3;
    if (!require(!rate_guard.validate(rate_frame, rate_options).ok, "third frame in rate window should be limited")) {
        return 12;
    }
    yuan::base::time::advance_steady_time_for_test(1000);
    if (!require(rate_guard.validate(rate_frame, rate_options).ok, "rate limit should reset after window")) {
        return 13;
    }
    yuan::base::time::reset_test_time();
    auto truncated = encoded;
    truncated.pop_back();
    if (!require(!decode_client_frame(truncated), "client frame should reject truncated payload")) {
        return 14;
    }
    encoded[0] = 0;
    if (!require(!decode_client_frame(encoded), "client frame should reject bad magic")) {
        return 9;
    }

    yuan::rpc::Bytes stream_frame_a;
    yuan::rpc::Bytes stream_frame_b;
    auto frame_b = frame;
    frame_b.header.sequence = 2;
    frame_b.payload = yuan::rpc::Codec<std::string>::encode("move:2,3");
    if (!require(encode_client_frame(frame, stream_frame_a) && encode_client_frame(frame_b, stream_frame_b), "stream frames should encode")) {
        return 15;
    }

    ClientFrameStreamDecoder partial_decoder;
    partial_decoder.append(stream_frame_a.data(), 12);
    auto partial = partial_decoder.next();
    if (!require(partial.status == ClientFrameStreamStatus::need_more, "partial client frame should wait for more bytes")) {
        return 16;
    }
    partial_decoder.append(stream_frame_a.data() + 12, stream_frame_a.size() - 12);
    auto complete = partial_decoder.next();
    if (!require(complete.status == ClientFrameStreamStatus::frame && complete.frame.has_value(), "complete client frame should decode after remaining bytes")) {
        return 17;
    }
    if (!require(complete.frame->payload == frame.payload && partial_decoder.buffered_size() == 0, "complete stream frame should match and consume buffer")) {
        return 18;
    }

    ClientFrameStreamDecoder sticky_decoder;
    yuan::rpc::Bytes sticky = stream_frame_a;
    sticky.insert(sticky.end(), stream_frame_b.begin(), stream_frame_b.end());
    sticky_decoder.append(sticky);
    auto first_sticky = sticky_decoder.next();
    if (!require(first_sticky.status == ClientFrameStreamStatus::frame && first_sticky.frame->header.sequence == 1, "first sticky client frame should decode")) {
        return 19;
    }
    auto second_sticky = sticky_decoder.next();
    if (!require(second_sticky.status == ClientFrameStreamStatus::frame && second_sticky.frame->header.sequence == 2, "second sticky client frame should decode")) {
        return 20;
    }
    if (!require(sticky_decoder.next().status == ClientFrameStreamStatus::need_more, "empty stream should need more bytes")) {
        return 21;
    }

    auto bad_magic_stream = stream_frame_a;
    bad_magic_stream[0] = 0;
    ClientFrameStreamDecoder bad_magic_decoder;
    bad_magic_decoder.append(bad_magic_stream);
    if (!require(bad_magic_decoder.next().status == ClientFrameStreamStatus::protocol_error, "stream decoder should reject bad magic")) {
        return 22;
    }

    auto bad_version_stream = stream_frame_a;
    bad_version_stream[7] = 2;
    ClientFrameStreamDecoder bad_version_decoder;
    bad_version_decoder.append(bad_version_stream);
    if (!require(bad_version_decoder.next().status == ClientFrameStreamStatus::protocol_error, "stream decoder should reject bad version")) {
        return 23;
    }

    ClientFrameValidationOptions small_stream_options;
    small_stream_options.max_frame_bytes = 4;
    ClientFrameStreamDecoder oversized_decoder(small_stream_options);
    oversized_decoder.append(stream_frame_a);
    if (!require(oversized_decoder.next().status == ClientFrameStreamStatus::protocol_error, "stream decoder should reject oversized declared payload")) {
        return 24;
    }

    ClientFrameStreamDecoder payload_wait_decoder;
    payload_wait_decoder.append(stream_frame_a.data(), stream_frame_a.size() - 1);
    if (!require(payload_wait_decoder.next().status == ClientFrameStreamStatus::need_more, "partial stream payload should wait for more bytes")) {
        return 25;
    }

    std::mt19937 rng(12345);
    for (int i = 0; i < 256; ++i) {
        yuan::rpc::Bytes fuzz(static_cast<std::size_t>(rng() % 96));
        for (auto &byte : fuzz) {
            byte = static_cast<std::uint8_t>(rng() & 0xff);
        }
        (void)decode_client_frame(fuzz);
        ClientFrameStreamDecoder fuzz_decoder;
        fuzz_decoder.append(fuzz);
        const auto fuzz_result = fuzz_decoder.next();
        if (!require(fuzz_result.status == ClientFrameStreamStatus::need_more || fuzz_result.status == ClientFrameStreamStatus::protocol_error,
                     "random malformed CS bytes should not decode as a valid frame")) {
            return 26;
        }
    }
    return EXIT_SUCCESS;
}
