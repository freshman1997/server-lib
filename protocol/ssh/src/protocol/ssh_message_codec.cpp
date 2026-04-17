#include "protocol/ssh_message_codec.h"
#include <cstring>

namespace yuan::net::ssh
{
    void SshMessageCodec::write_boolean(ByteBuffer & buf, bool value)
    {
        buf.append_u8(value ? 1 : 0);
    }

    void SshMessageCodec::write_uint32(ByteBuffer & buf, uint32_t value)
    {
        buf.append_u32(value);
    }

    void SshMessageCodec::write_uint64(ByteBuffer & buf, uint64_t value)
    {
        buf.append_u64(value);
    }

    void SshMessageCodec::write_string(ByteBuffer & buf, const std::string & value)
    {
        buf.append_u32(static_cast<uint32_t>(value.size()));
        buf.append(value.data(), value.size());
    }

    void SshMessageCodec::write_mpint(ByteBuffer & buf, const std::vector<uint8_t> & value)
    {
        size_t offset = 0;
        while (offset < value.size() && value[offset] == 0)
            ++offset;

        auto trimmed = value.size() - offset;
        bool needs_pad = (trimmed > 0 && (value[offset] & 0x80) != 0);

        uint32_t len = static_cast<uint32_t>(trimmed + (needs_pad ? 1 : 0));
        buf.append_u32(len);

        if (needs_pad)
            buf.append_u8(0x00);

        if (trimmed > 0)
            buf.append(value.data() + offset, trimmed);
    }

    void SshMessageCodec::write_name_list(ByteBuffer & buf, const std::string & value)
    {
        write_string(buf, value);
    }

    void SshMessageCodec::write_raw(ByteBuffer & buf, const uint8_t * data, size_t len)
    {
        buf.append_u32(static_cast<uint32_t>(len));
        buf.append(data, len);
    }

    bool SshMessageCodec::read_boolean(const uint8_t * data, size_t len, size_t & offset)
    {
        if (offset + 1 > len)
            return false;
        bool result = data[offset] != 0;
        offset += 1;
        return result;
    }

    uint32_t SshMessageCodec::read_uint32(const uint8_t * data, size_t len, size_t & offset)
    {
        if (offset + 4 > len)
            return 0;
        uint32_t result = (static_cast<uint32_t>(data[offset]) << 24) | (static_cast<uint32_t>(data[offset + 1]) << 16) | (static_cast<uint32_t>(data[offset + 2]) << 8) | static_cast<uint32_t>(data[offset + 3]);
        offset += 4;
        return result;
    }

    uint64_t SshMessageCodec::read_uint64(const uint8_t * data, size_t len, size_t & offset)
    {
        if (offset + 8 > len)
            return 0;
        uint64_t result = 0;
        for (int i = 0; i < 8; ++i)
            result = (result << 8) | data[offset + i];
        offset += 8;
        return result;
    }

    std::optional<std::string> SshMessageCodec::read_string(const uint8_t * data, size_t len, size_t & offset)
    {
        if (offset + 4 > len)
            return std::nullopt;
        uint32_t str_len = read_uint32(data, len, offset);
        if (offset + str_len > len)
            return std::nullopt;
        std::string result(reinterpret_cast<const char *>(data + offset), str_len);
        offset += str_len;
        return result;
    }

    std::optional<std::vector<uint8_t> > SshMessageCodec::read_mpint(const uint8_t * data, size_t len, size_t & offset)
    {
        if (offset + 4 > len)
            return std::nullopt;
        uint32_t mp_len = read_uint32(data, len, offset);
        if (offset + mp_len > len)
            return std::nullopt;
        std::vector<uint8_t> result(data + offset, data + offset + mp_len);
        offset += mp_len;

        size_t start = 0;
        while (start < result.size() && result[start] == 0)
            ++start;
        if (start > 0)
            result.erase(result.begin(), result.begin() + start);

        return result;
    }

    std::optional<std::string> SshMessageCodec::read_name_list(const uint8_t * data, size_t len, size_t & offset)
    {
        return read_string(data, len, offset);
    }

    std::optional<std::vector<uint8_t> > SshMessageCodec::read_raw(const uint8_t * data, size_t len, size_t & offset, size_t count)
    {
        if (offset + count > len)
            return std::nullopt;
        std::vector<uint8_t> result(data + offset, data + offset + count);
        offset += count;
        return result;
    }

    ByteBuffer SshMessageCodec::encode_kex_init(const SshKexInitMessage & msg)
    {
        ByteBuffer buf(512);
        buf.append_u8(static_cast<uint8_t>(SshMessageType::SSH_MSG_KEXINIT));
        buf.append(msg.cookie, 16);
        write_name_list(buf, msg.kex_algorithms);
        write_name_list(buf, msg.server_host_key_algorithms);
        write_name_list(buf, msg.encryption_algorithms_client_to_server);
        write_name_list(buf, msg.encryption_algorithms_server_to_client);
        write_name_list(buf, msg.mac_algorithms_client_to_server);
        write_name_list(buf, msg.mac_algorithms_server_to_client);
        write_name_list(buf, msg.compression_algorithms_client_to_server);
        write_name_list(buf, msg.compression_algorithms_server_to_client);
        write_name_list(buf, msg.languages_client_to_server);
        write_name_list(buf, msg.languages_server_to_client);
        write_boolean(buf, msg.first_kex_packet_follows);
        buf.append_u32(msg.reserved);
        return buf;
    }

    std::optional<SshKexInitMessage> SshMessageCodec::decode_kex_init(const uint8_t * data, size_t len)
    {
        if (len < 1 + 16)
            return std::nullopt;
        size_t offset = 0;
        if (data[offset] != static_cast<uint8_t>(SshMessageType::SSH_MSG_KEXINIT))
            return std::nullopt;
        ++offset;

        SshKexInitMessage msg;
        std::memcpy(msg.cookie, data + offset, 16);
        offset += 16;

        auto kex = read_name_list(data, len, offset);
        if (!kex)
            return std::nullopt;
        msg.kex_algorithms = std::move(*kex);

        auto host_key = read_name_list(data, len, offset);
        if (!host_key)
            return std::nullopt;
        msg.server_host_key_algorithms = std::move(*host_key);

        auto enc_c2s = read_name_list(data, len, offset);
        if (!enc_c2s)
            return std::nullopt;
        msg.encryption_algorithms_client_to_server = std::move(*enc_c2s);

        auto enc_s2c = read_name_list(data, len, offset);
        if (!enc_s2c)
            return std::nullopt;
        msg.encryption_algorithms_server_to_client = std::move(*enc_s2c);

        auto mac_c2s = read_name_list(data, len, offset);
        if (!mac_c2s)
            return std::nullopt;
        msg.mac_algorithms_client_to_server = std::move(*mac_c2s);

        auto mac_s2c = read_name_list(data, len, offset);
        if (!mac_s2c)
            return std::nullopt;
        msg.mac_algorithms_server_to_client = std::move(*mac_s2c);

        auto comp_c2s = read_name_list(data, len, offset);
        if (!comp_c2s)
            return std::nullopt;
        msg.compression_algorithms_client_to_server = std::move(*comp_c2s);

        auto comp_s2c = read_name_list(data, len, offset);
        if (!comp_s2c)
            return std::nullopt;
        msg.compression_algorithms_server_to_client = std::move(*comp_s2c);

        auto lang_c2s = read_name_list(data, len, offset);
        if (!lang_c2s)
            return std::nullopt;
        msg.languages_client_to_server = std::move(*lang_c2s);

        auto lang_s2c = read_name_list(data, len, offset);
        if (!lang_s2c)
            return std::nullopt;
        msg.languages_server_to_client = std::move(*lang_s2c);

        if (offset + 1 > len)
            return std::nullopt;
        msg.first_kex_packet_follows = read_boolean(data, len, offset);

        msg.reserved = read_uint32(data, len, offset);
        return msg;
    }

    ByteBuffer SshMessageCodec::encode_disconnect(const SshDisconnectMessage & msg)
    {
        ByteBuffer buf(64);
        buf.append_u8(static_cast<uint8_t>(SshMessageType::SSH_MSG_DISCONNECT));
        buf.append_u32(msg.reason_code);
        write_string(buf, msg.description);
        write_string(buf, msg.language);
        return buf;
    }

    std::optional<SshDisconnectMessage> SshMessageCodec::decode_disconnect(const uint8_t * data, size_t len)
    {
        if (len < 1)
            return std::nullopt;
        size_t offset = 0;
        if (data[offset] != static_cast<uint8_t>(SshMessageType::SSH_MSG_DISCONNECT))
            return std::nullopt;
        ++offset;

        SshDisconnectMessage msg;
        msg.reason_code = read_uint32(data, len, offset);

        auto desc = read_string(data, len, offset);
        if (!desc)
            return std::nullopt;
        msg.description = std::move(*desc);

        auto lang = read_string(data, len, offset);
        if (!lang)
            return std::nullopt;
        msg.language = std::move(*lang);

        return msg;
    }

    ByteBuffer SshMessageCodec::encode_service_request(const SshServiceRequestMessage & msg)
    {
        ByteBuffer buf(64);
        buf.append_u8(static_cast<uint8_t>(SshMessageType::SSH_MSG_SERVICE_REQUEST));
        write_string(buf, msg.service_name);
        return buf;
    }

    std::optional<SshServiceRequestMessage> SshMessageCodec::decode_service_request(const uint8_t * data, size_t len)
    {
        if (len < 1)
            return std::nullopt;
        size_t offset = 0;
        if (data[offset] != static_cast<uint8_t>(SshMessageType::SSH_MSG_SERVICE_REQUEST))
            return std::nullopt;
        ++offset;

        SshServiceRequestMessage msg;
        auto svc = read_string(data, len, offset);
        if (!svc)
            return std::nullopt;
        msg.service_name = std::move(*svc);
        return msg;
    }

    ByteBuffer SshMessageCodec::encode_service_accept(const SshServiceAcceptMessage & msg)
    {
        ByteBuffer buf(64);
        buf.append_u8(static_cast<uint8_t>(SshMessageType::SSH_MSG_SERVICE_ACCEPT));
        write_string(buf, msg.service_name);
        return buf;
    }

    std::optional<SshServiceAcceptMessage> SshMessageCodec::decode_service_accept(const uint8_t * data, size_t len)
    {
        if (len < 1)
            return std::nullopt;
        size_t offset = 0;
        if (data[offset] != static_cast<uint8_t>(SshMessageType::SSH_MSG_SERVICE_ACCEPT))
            return std::nullopt;
        ++offset;

        SshServiceAcceptMessage msg;
        auto svc = read_string(data, len, offset);
        if (!svc)
            return std::nullopt;
        msg.service_name = std::move(*svc);
        return msg;
    }

    ByteBuffer SshMessageCodec::encode_kex_ecdh_init(const SshKexEcdhInitMessage & msg)
    {
        ByteBuffer buf(256);
        buf.append_u8(static_cast<uint8_t>(SshMessageType::SSH_MSG_KEX_ECDH_INIT));
        write_raw(buf, msg.client_public_key.data(), msg.client_public_key.size());
        return buf;
    }

    std::optional<SshKexEcdhInitMessage> SshMessageCodec::decode_kex_ecdh_init(const uint8_t * data, size_t len)
    {
        if (len < 1)
            return std::nullopt;
        size_t offset = 0;
        if (data[offset] != static_cast<uint8_t>(SshMessageType::SSH_MSG_KEX_ECDH_INIT))
            return std::nullopt;
        ++offset;

        SshKexEcdhInitMessage msg;
        if (offset + 4 > len)
            return std::nullopt;
        uint32_t key_len = read_uint32(data, len, offset);
        auto key = read_raw(data, len, offset, key_len);
        if (!key)
            return std::nullopt;
        msg.client_public_key = std::move(*key);
        return msg;
    }

    ByteBuffer SshMessageCodec::encode_kex_ecdh_reply(const SshKexEcdhReplyMessage & msg)
    {
        ByteBuffer buf(512);
        buf.append_u8(static_cast<uint8_t>(SshMessageType::SSH_MSG_KEX_ECDH_REPLY));
        write_raw(buf, msg.host_key_blob.data(), msg.host_key_blob.size());
        write_raw(buf, msg.server_public_key.data(), msg.server_public_key.size());
        write_raw(buf, msg.signature.data(), msg.signature.size());
        return buf;
    }

    std::optional<SshKexEcdhReplyMessage> SshMessageCodec::decode_kex_ecdh_reply(const uint8_t * data, size_t len)
    {
        if (len < 1)
            return std::nullopt;
        size_t offset = 0;
        if (data[offset] != static_cast<uint8_t>(SshMessageType::SSH_MSG_KEX_ECDH_REPLY))
            return std::nullopt;
        ++offset;

        SshKexEcdhReplyMessage msg;

        if (offset + 4 > len)
            return std::nullopt;
        uint32_t hk_len = read_uint32(data, len, offset);
        auto hk = read_raw(data, len, offset, hk_len);
        if (!hk)
            return std::nullopt;
        msg.host_key_blob = std::move(*hk);

        if (offset + 4 > len)
            return std::nullopt;
        uint32_t spk_len = read_uint32(data, len, offset);
        auto spk = read_raw(data, len, offset, spk_len);
        if (!spk)
            return std::nullopt;
        msg.server_public_key = std::move(*spk);

        if (offset + 4 > len)
            return std::nullopt;
        uint32_t sig_len = read_uint32(data, len, offset);
        auto sig = read_raw(data, len, offset, sig_len);
        if (!sig)
            return std::nullopt;
        msg.signature = std::move(*sig);

        return msg;
    }

    ByteBuffer SshMessageCodec::encode_userauth_request(const SshUserauthRequestMessage & msg)
    {
        ByteBuffer buf(256);
        buf.append_u8(static_cast<uint8_t>(SshMessageType::SSH_MSG_USERAUTH_REQUEST));
        write_string(buf, msg.username);
        write_string(buf, msg.service_name);
        write_string(buf, msg.method_name);
        if (!msg.method_specific_data.empty())
            buf.append(msg.method_specific_data.data(), msg.method_specific_data.size());
        return buf;
    }

    std::optional<SshUserauthRequestMessage> SshMessageCodec::decode_userauth_request(const uint8_t * data, size_t len)
    {
        if (len < 1)
            return std::nullopt;
        size_t offset = 0;
        if (data[offset] != static_cast<uint8_t>(SshMessageType::SSH_MSG_USERAUTH_REQUEST))
            return std::nullopt;
        ++offset;

        SshUserauthRequestMessage msg;

        auto user = read_string(data, len, offset);
        if (!user)
            return std::nullopt;
        msg.username = std::move(*user);

        auto svc = read_string(data, len, offset);
        if (!svc)
            return std::nullopt;
        msg.service_name = std::move(*svc);

        auto method = read_string(data, len, offset);
        if (!method)
            return std::nullopt;
        msg.method_name = std::move(*method);

        if (offset < len)
            msg.method_specific_data.assign(data + offset, data + len);

        return msg;
    }

    ByteBuffer SshMessageCodec::encode_userauth_failure(const SshUserauthFailureMessage & msg)
    {
        ByteBuffer buf(128);
        buf.append_u8(static_cast<uint8_t>(SshMessageType::SSH_MSG_USERAUTH_FAILURE));
        write_name_list(buf, msg.auth_methods_that_can_continue);
        write_boolean(buf, msg.partial_success);
        return buf;
    }

    std::optional<SshUserauthFailureMessage> SshMessageCodec::decode_userauth_failure(const uint8_t * data, size_t len)
    {
        if (len < 1)
            return std::nullopt;
        size_t offset = 0;
        if (data[offset] != static_cast<uint8_t>(SshMessageType::SSH_MSG_USERAUTH_FAILURE))
            return std::nullopt;
        ++offset;

        SshUserauthFailureMessage msg;

        auto methods = read_name_list(data, len, offset);
        if (!methods)
            return std::nullopt;
        msg.auth_methods_that_can_continue = std::move(*methods);

        msg.partial_success = read_boolean(data, len, offset);
        return msg;
    }

    ByteBuffer SshMessageCodec::encode_userauth_banner(const SshUserauthBannerMessage & msg)
    {
        ByteBuffer buf(128);
        buf.append_u8(static_cast<uint8_t>(SshMessageType::SSH_MSG_USERAUTH_BANNER));
        write_string(buf, msg.message);
        write_string(buf, msg.language);
        return buf;
    }

    std::optional<SshUserauthBannerMessage> SshMessageCodec::decode_userauth_banner(const uint8_t * data, size_t len)
    {
        if (len < 1)
            return std::nullopt;
        size_t offset = 0;
        if (data[offset] != static_cast<uint8_t>(SshMessageType::SSH_MSG_USERAUTH_BANNER))
            return std::nullopt;
        ++offset;

        SshUserauthBannerMessage msg;

        auto message = read_string(data, len, offset);
        if (!message)
            return std::nullopt;
        msg.message = std::move(*message);

        auto lang = read_string(data, len, offset);
        if (!lang)
            return std::nullopt;
        msg.language = std::move(*lang);

        return msg;
    }

    ByteBuffer SshMessageCodec::encode_userauth_pk_ok(const SshUserauthPkOkMessage & msg)
    {
        ByteBuffer buf(128);
        buf.append_u8(static_cast<uint8_t>(SshMessageType::SSH_MSG_USERAUTH_PK_OK));
        write_string(buf, msg.algorithm_name);
        write_raw(buf, msg.public_key_blob.data(), msg.public_key_blob.size());
        return buf;
    }

    std::optional<SshUserauthPkOkMessage> SshMessageCodec::decode_userauth_pk_ok(const uint8_t * data, size_t len)
    {
        if (len < 1)
            return std::nullopt;
        size_t offset = 0;
        if (data[offset] != static_cast<uint8_t>(SshMessageType::SSH_MSG_USERAUTH_PK_OK))
            return std::nullopt;
        ++offset;

        SshUserauthPkOkMessage msg;

        auto alg = read_string(data, len, offset);
        if (!alg)
            return std::nullopt;
        msg.algorithm_name = std::move(*alg);

        if (offset + 4 > len)
            return std::nullopt;
        uint32_t blob_len = read_uint32(data, len, offset);
        auto blob = read_raw(data, len, offset, blob_len);
        if (!blob)
            return std::nullopt;
        msg.public_key_blob = std::move(*blob);

        return msg;
    }

    ByteBuffer SshMessageCodec::encode_userauth_info_request(const SshUserauthInfoRequestMessage & msg)
    {
        ByteBuffer buf(256);
        buf.append_u8(static_cast<uint8_t>(SshMessageType::SSH_MSG_USERAUTH_INFO_REQUEST));
        write_string(buf, msg.name);
        write_string(buf, msg.instruction);
        write_string(buf, msg.language);
        buf.append_u32(static_cast<uint32_t>(msg.prompts.size()));
        for (const auto &p : msg.prompts) {
            write_string(buf, p.prompt);
            write_boolean(buf, p.echo);
        }
        return buf;
    }

    std::optional<SshUserauthInfoRequestMessage> SshMessageCodec::decode_userauth_info_request(const uint8_t * data, size_t len)
    {
        if (len < 1)
            return std::nullopt;
        size_t offset = 0;
        if (data[offset] != static_cast<uint8_t>(SshMessageType::SSH_MSG_USERAUTH_INFO_REQUEST))
            return std::nullopt;
        ++offset;

        SshUserauthInfoRequestMessage msg;

        auto name = read_string(data, len, offset);
        if (!name)
            return std::nullopt;
        msg.name = std::move(*name);

        auto instr = read_string(data, len, offset);
        if (!instr)
            return std::nullopt;
        msg.instruction = std::move(*instr);

        auto lang = read_string(data, len, offset);
        if (!lang)
            return std::nullopt;
        msg.language = std::move(*lang);

        uint32_t num_prompts = read_uint32(data, len, offset);
        for (uint32_t i = 0; i < num_prompts; ++i) {
            SshAuthPrompt prompt;
            auto p = read_string(data, len, offset);
            if (!p)
                return std::nullopt;
            prompt.prompt = std::move(*p);
            prompt.echo = read_boolean(data, len, offset);
            msg.prompts.push_back(std::move(prompt));
        }

        return msg;
    }

    ByteBuffer SshMessageCodec::encode_userauth_info_response(const SshUserauthInfoResponseMessage & msg)
    {
        ByteBuffer buf(256);
        buf.append_u8(static_cast<uint8_t>(SshMessageType::SSH_MSG_USERAUTH_INFO_RESPONSE));
        buf.append_u32(static_cast<uint32_t>(msg.responses.size()));
        for (const auto &r : msg.responses)
            write_string(buf, r);
        return buf;
    }

    std::optional<SshUserauthInfoResponseMessage> SshMessageCodec::decode_userauth_info_response(const uint8_t * data, size_t len)
    {
        if (len < 1)
            return std::nullopt;
        size_t offset = 0;
        if (data[offset] != static_cast<uint8_t>(SshMessageType::SSH_MSG_USERAUTH_INFO_RESPONSE))
            return std::nullopt;
        ++offset;

        SshUserauthInfoResponseMessage msg;
        uint32_t num = read_uint32(data, len, offset);
        for (uint32_t i = 0; i < num; ++i) {
            auto r = read_string(data, len, offset);
            if (!r)
                return std::nullopt;
            msg.responses.push_back(std::move(*r));
        }

        return msg;
    }

    ByteBuffer SshMessageCodec::encode_channel_open(const SshChannelOpenMessage & msg)
    {
        ByteBuffer buf(128);
        buf.append_u8(static_cast<uint8_t>(SshMessageType::SSH_MSG_CHANNEL_OPEN));
        write_string(buf, msg.channel_type);
        buf.append_u32(msg.sender_channel);
        buf.append_u32(msg.initial_window_size);
        buf.append_u32(msg.maximum_packet_size);
        if (!msg.type_specific_data.empty())
            buf.append(msg.type_specific_data.data(), msg.type_specific_data.size());
        return buf;
    }

    std::optional<SshChannelOpenMessage> SshMessageCodec::decode_channel_open(const uint8_t * data, size_t len)
    {
        if (len < 1 + 12)
            return std::nullopt;
        size_t offset = 0;
        if (data[offset] != static_cast<uint8_t>(SshMessageType::SSH_MSG_CHANNEL_OPEN))
            return std::nullopt;
        ++offset;

        SshChannelOpenMessage msg;

        auto type = read_string(data, len, offset);
        if (!type)
            return std::nullopt;
        msg.channel_type = std::move(*type);

        msg.sender_channel = read_uint32(data, len, offset);
        msg.initial_window_size = read_uint32(data, len, offset);
        msg.maximum_packet_size = read_uint32(data, len, offset);

        if (offset < len)
            msg.type_specific_data.assign(data + offset, data + len);

        return msg;
    }

    ByteBuffer SshMessageCodec::encode_channel_open_confirmation(const SshChannelOpenConfirmationMessage & msg)
    {
        ByteBuffer buf(32);
        buf.append_u8(static_cast<uint8_t>(SshMessageType::SSH_MSG_CHANNEL_OPEN_CONFIRMATION));
        buf.append_u32(msg.recipient_channel);
        buf.append_u32(msg.sender_channel);
        buf.append_u32(msg.initial_window_size);
        buf.append_u32(msg.maximum_packet_size);
        return buf;
    }

    std::optional<SshChannelOpenConfirmationMessage> SshMessageCodec::decode_channel_open_confirmation(const uint8_t * data, size_t len)
    {
        if (len < 1 + 16)
            return std::nullopt;
        size_t offset = 0;
        if (data[offset] != static_cast<uint8_t>(SshMessageType::SSH_MSG_CHANNEL_OPEN_CONFIRMATION))
            return std::nullopt;
        ++offset;

        SshChannelOpenConfirmationMessage msg;
        msg.recipient_channel = read_uint32(data, len, offset);
        msg.sender_channel = read_uint32(data, len, offset);
        msg.initial_window_size = read_uint32(data, len, offset);
        msg.maximum_packet_size = read_uint32(data, len, offset);
        return msg;
    }

    ByteBuffer SshMessageCodec::encode_channel_open_failure(const SshChannelOpenFailureMessage & msg)
    {
        ByteBuffer buf(128);
        buf.append_u8(static_cast<uint8_t>(SshMessageType::SSH_MSG_CHANNEL_OPEN_FAILURE));
        buf.append_u32(msg.recipient_channel);
        buf.append_u32(msg.reason_code);
        write_string(buf, msg.description);
        write_string(buf, msg.language);
        return buf;
    }

    std::optional<SshChannelOpenFailureMessage> SshMessageCodec::decode_channel_open_failure(const uint8_t * data, size_t len)
    {
        if (len < 1 + 8)
            return std::nullopt;
        size_t offset = 0;
        if (data[offset] != static_cast<uint8_t>(SshMessageType::SSH_MSG_CHANNEL_OPEN_FAILURE))
            return std::nullopt;
        ++offset;

        SshChannelOpenFailureMessage msg;
        msg.recipient_channel = read_uint32(data, len, offset);
        msg.reason_code = read_uint32(data, len, offset);

        auto desc = read_string(data, len, offset);
        if (!desc)
            return std::nullopt;
        msg.description = std::move(*desc);

        auto lang = read_string(data, len, offset);
        if (!lang)
            return std::nullopt;
        msg.language = std::move(*lang);

        return msg;
    }

    ByteBuffer SshMessageCodec::encode_channel_window_adjust(const SshChannelWindowAdjustMessage & msg)
    {
        ByteBuffer buf(16);
        buf.append_u8(static_cast<uint8_t>(SshMessageType::SSH_MSG_CHANNEL_WINDOW_ADJUST));
        buf.append_u32(msg.recipient_channel);
        buf.append_u32(msg.bytes_to_add);
        return buf;
    }

    std::optional<SshChannelWindowAdjustMessage> SshMessageCodec::decode_channel_window_adjust(const uint8_t * data, size_t len)
    {
        if (len < 1 + 8)
            return std::nullopt;
        size_t offset = 0;
        if (data[offset] != static_cast<uint8_t>(SshMessageType::SSH_MSG_CHANNEL_WINDOW_ADJUST))
            return std::nullopt;
        ++offset;

        SshChannelWindowAdjustMessage msg;
        msg.recipient_channel = read_uint32(data, len, offset);
        msg.bytes_to_add = read_uint32(data, len, offset);
        return msg;
    }

    ByteBuffer SshMessageCodec::encode_channel_data(const SshChannelDataMessage & msg)
    {
        ByteBuffer buf(16 + msg.data.size());
        buf.append_u8(static_cast<uint8_t>(SshMessageType::SSH_MSG_CHANNEL_DATA));
        buf.append_u32(msg.recipient_channel);
        write_raw(buf, msg.data.data(), msg.data.size());
        return buf;
    }

    std::optional<SshChannelDataMessage> SshMessageCodec::decode_channel_data(const uint8_t * data, size_t len)
    {
        if (len < 1 + 8)
            return std::nullopt;
        size_t offset = 0;
        if (data[offset] != static_cast<uint8_t>(SshMessageType::SSH_MSG_CHANNEL_DATA))
            return std::nullopt;
        ++offset;

        SshChannelDataMessage msg;
        msg.recipient_channel = read_uint32(data, len, offset);

        if (offset + 4 > len)
            return std::nullopt;
        uint32_t data_len = read_uint32(data, len, offset);
        auto d = read_raw(data, len, offset, data_len);
        if (!d)
            return std::nullopt;
        msg.data = std::move(*d);
        return msg;
    }

    ByteBuffer SshMessageCodec::encode_channel_extended_data(const SshChannelExtendedDataMessage & msg)
    {
        ByteBuffer buf(16 + msg.data.size());
        buf.append_u8(static_cast<uint8_t>(SshMessageType::SSH_MSG_CHANNEL_EXTENDED_DATA));
        buf.append_u32(msg.recipient_channel);
        buf.append_u32(msg.data_type_code);
        write_raw(buf, msg.data.data(), msg.data.size());
        return buf;
    }

    std::optional<SshChannelExtendedDataMessage> SshMessageCodec::decode_channel_extended_data(const uint8_t * data, size_t len)
    {
        if (len < 1 + 12)
            return std::nullopt;
        size_t offset = 0;
        if (data[offset] != static_cast<uint8_t>(SshMessageType::SSH_MSG_CHANNEL_EXTENDED_DATA))
            return std::nullopt;
        ++offset;

        SshChannelExtendedDataMessage msg;
        msg.recipient_channel = read_uint32(data, len, offset);
        msg.data_type_code = read_uint32(data, len, offset);

        if (offset + 4 > len)
            return std::nullopt;
        uint32_t data_len = read_uint32(data, len, offset);
        auto d = read_raw(data, len, offset, data_len);
        if (!d)
            return std::nullopt;
        msg.data = std::move(*d);
        return msg;
    }

    ByteBuffer SshMessageCodec::encode_channel_eof(const SshChannelEofMessage & msg)
    {
        ByteBuffer buf(8);
        buf.append_u8(static_cast<uint8_t>(SshMessageType::SSH_MSG_CHANNEL_EOF));
        buf.append_u32(msg.recipient_channel);
        return buf;
    }

    std::optional<SshChannelEofMessage> SshMessageCodec::decode_channel_eof(const uint8_t * data, size_t len)
    {
        if (len < 1 + 4)
            return std::nullopt;
        size_t offset = 0;
        if (data[offset] != static_cast<uint8_t>(SshMessageType::SSH_MSG_CHANNEL_EOF))
            return std::nullopt;
        ++offset;

        SshChannelEofMessage msg;
        msg.recipient_channel = read_uint32(data, len, offset);
        return msg;
    }

    ByteBuffer SshMessageCodec::encode_channel_close(const SshChannelCloseMessage & msg)
    {
        ByteBuffer buf(8);
        buf.append_u8(static_cast<uint8_t>(SshMessageType::SSH_MSG_CHANNEL_CLOSE));
        buf.append_u32(msg.recipient_channel);
        return buf;
    }

    std::optional<SshChannelCloseMessage> SshMessageCodec::decode_channel_close(const uint8_t * data, size_t len)
    {
        if (len < 1 + 4)
            return std::nullopt;
        size_t offset = 0;
        if (data[offset] != static_cast<uint8_t>(SshMessageType::SSH_MSG_CHANNEL_CLOSE))
            return std::nullopt;
        ++offset;

        SshChannelCloseMessage msg;
        msg.recipient_channel = read_uint32(data, len, offset);
        return msg;
    }

    ByteBuffer SshMessageCodec::encode_channel_request(const SshChannelRequestMessage & msg)
    {
        ByteBuffer buf(64 + msg.request_specific_data.size());
        buf.append_u8(static_cast<uint8_t>(SshMessageType::SSH_MSG_CHANNEL_REQUEST));
        buf.append_u32(msg.recipient_channel);
        write_string(buf, msg.request_type);
        write_boolean(buf, msg.want_reply);
        if (!msg.request_specific_data.empty())
            buf.append(msg.request_specific_data.data(), msg.request_specific_data.size());
        return buf;
    }

    std::optional<SshChannelRequestMessage> SshMessageCodec::decode_channel_request(const uint8_t * data, size_t len)
    {
        if (len < 1 + 4)
            return std::nullopt;
        size_t offset = 0;
        if (data[offset] != static_cast<uint8_t>(SshMessageType::SSH_MSG_CHANNEL_REQUEST))
            return std::nullopt;
        ++offset;

        SshChannelRequestMessage msg;
        msg.recipient_channel = read_uint32(data, len, offset);

        auto req_type = read_string(data, len, offset);
        if (!req_type)
            return std::nullopt;
        msg.request_type = std::move(*req_type);

        msg.want_reply = read_boolean(data, len, offset);

        if (offset < len)
            msg.request_specific_data.assign(data + offset, data + len);

        return msg;
    }

    ByteBuffer SshMessageCodec::encode_channel_success(const SshChannelSuccessMessage & msg)
    {
        ByteBuffer buf(8);
        buf.append_u8(static_cast<uint8_t>(SshMessageType::SSH_MSG_CHANNEL_SUCCESS));
        buf.append_u32(msg.recipient_channel);
        return buf;
    }

    std::optional<SshChannelSuccessMessage> SshMessageCodec::decode_channel_success(const uint8_t * data, size_t len)
    {
        if (len < 1 + 4)
            return std::nullopt;
        size_t offset = 0;
        if (data[offset] != static_cast<uint8_t>(SshMessageType::SSH_MSG_CHANNEL_SUCCESS))
            return std::nullopt;
        ++offset;

        SshChannelSuccessMessage msg;
        msg.recipient_channel = read_uint32(data, len, offset);
        return msg;
    }

    ByteBuffer SshMessageCodec::encode_channel_failure(const SshChannelFailureMessage & msg)
    {
        ByteBuffer buf(8);
        buf.append_u8(static_cast<uint8_t>(SshMessageType::SSH_MSG_CHANNEL_FAILURE));
        buf.append_u32(msg.recipient_channel);
        return buf;
    }

    std::optional<SshChannelFailureMessage> SshMessageCodec::decode_channel_failure(const uint8_t * data, size_t len)
    {
        if (len < 1 + 4)
            return std::nullopt;
        size_t offset = 0;
        if (data[offset] != static_cast<uint8_t>(SshMessageType::SSH_MSG_CHANNEL_FAILURE))
            return std::nullopt;
        ++offset;

        SshChannelFailureMessage msg;
        msg.recipient_channel = read_uint32(data, len, offset);
        return msg;
    }

    ByteBuffer SshMessageCodec::encode_global_request(const SshGlobalRequestMessage & msg)
    {
        ByteBuffer buf(64 + msg.request_specific_data.size());
        buf.append_u8(static_cast<uint8_t>(SshMessageType::SSH_MSG_GLOBAL_REQUEST));
        write_string(buf, msg.request_name);
        write_boolean(buf, msg.want_reply);
        if (!msg.request_specific_data.empty())
            buf.append(msg.request_specific_data.data(), msg.request_specific_data.size());
        return buf;
    }

    std::optional<SshGlobalRequestMessage> SshMessageCodec::decode_global_request(const uint8_t * data, size_t len)
    {
        if (len < 1)
            return std::nullopt;
        size_t offset = 0;
        if (data[offset] != static_cast<uint8_t>(SshMessageType::SSH_MSG_GLOBAL_REQUEST))
            return std::nullopt;
        ++offset;

        SshGlobalRequestMessage msg;

        auto name = read_string(data, len, offset);
        if (!name)
            return std::nullopt;
        msg.request_name = std::move(*name);

        msg.want_reply = read_boolean(data, len, offset);

        if (offset < len)
            msg.request_specific_data.assign(data + offset, data + len);

        return msg;
    }

    ByteBuffer SshMessageCodec::encode_newkeys()
    {
        ByteBuffer buf(4);
        buf.append_u8(static_cast<uint8_t>(SshMessageType::SSH_MSG_NEWKEYS));
        return buf;
    }

    ByteBuffer SshMessageCodec::encode_userauth_success()
    {
        ByteBuffer buf(4);
        buf.append_u8(static_cast<uint8_t>(SshMessageType::SSH_MSG_USERAUTH_SUCCESS));
        return buf;
    }

    ByteBuffer SshMessageCodec::encode_ignore(const std::vector<uint8_t> & data)
    {
        ByteBuffer buf(8 + data.size());
        buf.append_u8(static_cast<uint8_t>(SshMessageType::SSH_MSG_IGNORE));
        write_raw(buf, data.data(), data.size());
        return buf;
    }

    ByteBuffer SshMessageCodec::encode_debug(const SshDebugMessage & msg)
    {
        ByteBuffer buf(64);
        buf.append_u8(static_cast<uint8_t>(SshMessageType::SSH_MSG_DEBUG));
        write_boolean(buf, msg.always_display);
        write_string(buf, msg.message);
        write_string(buf, msg.language);
        return buf;
    }

    ByteBuffer SshMessageCodec::encode_unimplemented(uint32_t seq)
    {
        ByteBuffer buf(8);
        buf.append_u8(static_cast<uint8_t>(SshMessageType::SSH_MSG_UNIMPLEMENTED));
        buf.append_u32(seq);
        return buf;
    }
}
