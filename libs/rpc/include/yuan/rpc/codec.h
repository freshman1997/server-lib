#ifndef YUAN_RPC_CODEC_H
#define YUAN_RPC_CODEC_H

#include "types.h"

#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace yuan::rpc
{
    template<typename T>
    struct Codec
    {
        static Bytes encode(const T &value)
        {
            static_assert(sizeof(T) == 0, "yuan::rpc::Codec<T> must be specialized for this type");
            (void)value;
            return {};
        }

        static T decode(const Bytes &bytes)
        {
            static_assert(sizeof(T) == 0, "yuan::rpc::Codec<T> must be specialized for this type");
            (void)bytes;
            return {};
        }
    };

    template<typename T>
    struct CodecTraits
    {
        static constexpr Serialization serialization = Serialization::raw;
    };

    template<>
    struct Codec<Bytes>
    {
        static Bytes encode(const Bytes &value)
        {
            return value;
        }

        static Bytes decode(const Bytes &bytes)
        {
            return bytes;
        }
    };

    template<>
    struct CodecTraits<Bytes>
    {
        static constexpr Serialization serialization = Serialization::raw;
    };

    template<>
    struct Codec<std::string>
    {
        static Bytes encode(std::string_view value)
        {
            return Bytes(value.begin(), value.end());
        }

        static std::string decode(const Bytes &bytes)
        {
            return std::string(bytes.begin(), bytes.end());
        }
    };

    template<>
    struct CodecTraits<std::string>
    {
        static constexpr Serialization serialization = Serialization::raw;
    };

    struct JsonText
    {
        std::string value;
    };

    template<>
    struct Codec<JsonText>
    {
        static Bytes encode(const JsonText &value)
        {
            return Bytes(value.value.begin(), value.value.end());
        }

        static JsonText decode(const Bytes &bytes)
        {
            return JsonText{std::string(bytes.begin(), bytes.end())};
        }
    };

    template<>
    struct CodecTraits<JsonText>
    {
        static constexpr Serialization serialization = Serialization::json;
    };

    struct ProtobufBytes
    {
        Bytes value;
    };

    template<>
    struct Codec<ProtobufBytes>
    {
        static Bytes encode(const ProtobufBytes &value)
        {
            return value.value;
        }

        static ProtobufBytes decode(const Bytes &bytes)
        {
            return ProtobufBytes{bytes};
        }
    };

    template<>
    struct CodecTraits<ProtobufBytes>
    {
        static constexpr Serialization serialization = Serialization::protobuf;
    };

    struct FlatBufferBytes
    {
        Bytes value;
    };

    template<>
    struct Codec<FlatBufferBytes>
    {
        static Bytes encode(const FlatBufferBytes &value)
        {
            return value.value;
        }

        static FlatBufferBytes decode(const Bytes &bytes)
        {
            return FlatBufferBytes{bytes};
        }
    };

    template<>
    struct CodecTraits<FlatBufferBytes>
    {
        static constexpr Serialization serialization = Serialization::flatbuffers;
    };

    struct MsgPackBytes
    {
        Bytes value;
    };

    template<>
    struct Codec<MsgPackBytes>
    {
        static Bytes encode(const MsgPackBytes &value)
        {
            return value.value;
        }

        static MsgPackBytes decode(const Bytes &bytes)
        {
            return MsgPackBytes{bytes};
        }
    };

    template<>
    struct CodecTraits<MsgPackBytes>
    {
        static constexpr Serialization serialization = Serialization::msgpack;
    };
}

#endif
