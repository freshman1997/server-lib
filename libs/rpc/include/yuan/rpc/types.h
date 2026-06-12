#ifndef YUAN_RPC_TYPES_H
#define YUAN_RPC_TYPES_H

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace yuan::rpc
{
    using RequestId = std::uint64_t;
    using ContinuationId = std::uint64_t;
    using CoroutineId = ContinuationId;
    using ServiceId = std::uint32_t;
    using MethodId = std::uint32_t;
    using NodeId = std::uint64_t;
    using Bytes = std::vector<std::uint8_t>;
    using Metadata = std::unordered_map<std::string, std::string>;

    namespace metadata_key
    {
        inline constexpr std::string_view trace_id = "trace_id";
        inline constexpr std::string_view span_id = "span_id";
        inline constexpr std::string_view tenant_id = "tenant_id";
        inline constexpr std::string_view auth_token = "auth_token";
        inline constexpr std::string_view locale = "locale";
        inline constexpr std::string_view user_agent = "user_agent";
    }

    enum class Serialization : std::uint8_t
    {
        raw = 0,
        json = 1,
        protobuf = 2,
        flatbuffers = 3,
        msgpack = 4
    };

    enum class Compression : std::uint8_t
    {
        none = 0,
        zstd = 1,
        lz4 = 2,
        gzip = 3
    };

    enum class Encryption : std::uint8_t
    {
        none = 0,
        xor_stream = 1,
        aes_128_gcm = 2,
        aes_256_gcm = 3,
        chacha20_poly1305 = 4
    };

    enum class MessageKind : std::uint8_t
    {
        request,
        response,
        push
    };

    enum class RpcStatus : std::uint16_t
    {
        ok = 0,
        not_found = 1,
        timeout = 2,
        canceled = 3,
        bad_request = 4,
        unavailable = 5,
        internal_error = 6
    };

    struct Route
    {
        ServiceId service = 0;
        MethodId method = 0;
        std::string name;

        [[nodiscard]] bool valid() const
        {
            return service != 0 || !name.empty();
        }
    };

    struct Peer
    {
        NodeId node = 0;
        std::string endpoint;
    };

    struct Message
    {
        MessageKind kind = MessageKind::request;
        RequestId request_id = 0;
        CoroutineId coroutine_id = 0;
        Route route;
        Peer source;
        Peer target;
        Serialization serialization = Serialization::raw;
        Compression compression = Compression::none;
        Encryption encryption = Encryption::none;
        std::uint32_t key_id = 0;
        std::uint64_t nonce = 0;
        Metadata metadata;
        Bytes payload;

        [[nodiscard]] ContinuationId continuation_id() const
        {
            return coroutine_id;
        }

        void set_continuation_id(ContinuationId id)
        {
            coroutine_id = id;
        }
    };

    struct Response
    {
        RequestId request_id = 0;
        CoroutineId coroutine_id = 0;
        RpcStatus status = RpcStatus::ok;
        std::string error;
        Serialization serialization = Serialization::raw;
        Compression compression = Compression::none;
        Encryption encryption = Encryption::none;
        std::uint32_t key_id = 0;
        std::uint64_t nonce = 0;
        Metadata metadata;
        Bytes payload;

        [[nodiscard]] ContinuationId continuation_id() const
        {
            return coroutine_id;
        }

        void set_continuation_id(ContinuationId id)
        {
            coroutine_id = id;
        }
    };

    struct CallOptions
    {
        std::chrono::milliseconds timeout{3000};
        CoroutineId coroutine_id = 0;
        Serialization serialization = Serialization::raw;
        Compression compression = Compression::none;
        Encryption encryption = Encryption::none;
        std::uint32_t key_id = 0;
        std::uint64_t nonce = 0;
        Metadata metadata;

        [[nodiscard]] ContinuationId continuation_id() const
        {
            return coroutine_id;
        }

        void set_continuation_id(ContinuationId id)
        {
            coroutine_id = id;
        }
    };

    struct RpcContext
    {
        RequestId request_id = 0;
        ContinuationId continuation_id = 0;
        Peer source;
        Peer target;
        Metadata metadata;

        [[nodiscard]] std::optional<std::string> metadata_value(std::string_view key) const;
    };

    RpcContext context_from(const Message &message);

    RpcContext context_from(const Response &response);

    using RequestHandler = std::function<Response(const Message &)>;
    using ResponseHandler = std::function<void(Response)>;

    class RpcError : public std::runtime_error
    {
    public:
        RpcError(RpcStatus status, std::string message);

        [[nodiscard]] RpcStatus status() const;

    private:
        static std::string default_message(RpcStatus status);

        RpcStatus status_ = RpcStatus::internal_error;
    };

    std::string_view to_string(RpcStatus status);

    std::string route_key(const Route &route);
}

#endif
