#include "nat/metadata_manager.h"
#include "peer_wire/peer_connection.h"
#include "structure/bencoding.h"
#include "utils.h"
#include <cstring>
#include <algorithm>

namespace yuan::net::bit_torrent
{

    MetadataManager::MetadataManager()
        : next_ext_id_(1)
    {
    }

    MetadataManager::~MetadataManager() {}

    void MetadataManager::init(const std::vector<uint8_t> &info_hash)
    {
        info_hash_ = info_hash;
        our_ut_metadata_ext_id_ = next_ext_id_++;
        our_ut_metadata_id_str_ = std::to_string(our_ut_metadata_ext_id_);
    }

    void MetadataManager::set_metadata_size(int32_t size)
    {
        if (metadata_size_ > 0 && metadata_size_ != size)
            return;
        metadata_size_ = size;
        metadata_.resize(size, 0);
    }

    std::vector<uint8_t> MetadataManager::build_ext_handshake() const
    {
        std::string dict;
        dict += "1:m";

        dict += "d";
        dict += "10:ut_metadata";
        dict += "i";
        dict += our_ut_metadata_id_str_;
        dict += "e";
        dict += "e";

        if (metadata_size_ > 0)
        {
            dict += "13:metadata_size";
            dict += "i";
            dict += std::to_string(metadata_size_);
            dict += "e";
        }

        return std::vector<uint8_t>(dict.begin(), dict.end());
    }

    bool MetadataManager::on_extended_message(PeerConnection *peer,
                                               const std::string &peer_key,
                                               uint8_t ext_id,
                                               const uint8_t *payload,
                                               size_t len)
    {
        if (ext_id == 0)
        {
            return parse_ext_handshake(peer_key, payload, len);
        }

        auto it = peer_states_.find(peer_key);
        if (it == peer_states_.end() || !it->second.supports_metadata)
            return false;

        if (ext_id != static_cast<uint8_t>(it->second.ut_metadata_msg_id))
            return false;

        std::string str(reinterpret_cast<const char *>(payload), len);
        auto *parsed = BencodingDataConverter::parse(str);
        if (!parsed || parsed->type_ != DataType::dictionary_)
        {
            delete parsed;
            return false;
        }

        auto *dict = static_cast<DicttionaryData *>(parsed);

        int64_t msg_type = -1;
        if (auto *v = dict->get_val("msg_type"); v && v->type_ == DataType::integer_)
            msg_type = static_cast<IntegerData *>(v)->get_data();

        bool handled = false;
        if (msg_type == 0)
        {
            handled = handle_metadata_request(peer, peer_key, payload, len);
        }
        else if (msg_type == 1)
        {
            handled = handle_metadata_data(peer_key, payload, len);
        }
        else if (msg_type == 2)
        {
            handled = handle_metadata_reject(peer_key, payload, len);
        }

        delete parsed;
        return handled;
    }

    bool MetadataManager::parse_ext_handshake(const std::string &peer_key,
                                               const uint8_t *data, size_t len)
    {
        std::string str(reinterpret_cast<const char *>(data), len);
        auto *parsed = BencodingDataConverter::parse(str);
        if (!parsed || parsed->type_ != DataType::dictionary_)
        {
            delete parsed;
            return false;
        }

        auto *dict = static_cast<DicttionaryData *>(parsed);

        PeerMetadataState state;

        if (auto *m = dict->get_val("m"); m && m->type_ == DataType::dictionary_)
        {
            auto *m_dict = static_cast<DicttionaryData *>(m);
            if (auto *ut_meta = m_dict->get_val("ut_metadata"); ut_meta && ut_meta->type_ == DataType::integer_)
            {
                state.supports_metadata = true;
                state.ut_metadata_msg_id = static_cast<int>(static_cast<IntegerData *>(ut_meta)->get_data());
            }
        }

        if (auto *v = dict->get_val("metadata_size"); v && v->type_ == DataType::integer_)
        {
            state.metadata_size = static_cast<int32_t>(static_cast<IntegerData *>(v)->get_data());
            if (state.metadata_size > 0 && metadata_size_ == 0)
            {
                set_metadata_size(state.metadata_size);
            }
        }

        peer_states_[peer_key] = state;

        delete parsed;
        return state.supports_metadata;
    }

    bool MetadataManager::handle_metadata_data(const std::string &peer_key,
                                                const uint8_t *data,
                                                size_t len)
    {
        if (metadata_complete_)
            return true;

        std::string str(reinterpret_cast<const char *>(data), len);
        auto *parsed = BencodingDataConverter::parse(str);
        if (!parsed || parsed->type_ != DataType::dictionary_)
        {
            delete parsed;
            return false;
        }

        auto *dict = static_cast<DicttionaryData *>(parsed);

        int64_t msg_type = -1;
        int64_t piece_index = -1;
        int64_t total_size = 0;

        if (auto *v = dict->get_val("msg_type"); v && v->type_ == DataType::integer_)
            msg_type = static_cast<IntegerData *>(v)->get_data();
        if (auto *v = dict->get_val("piece"); v && v->type_ == DataType::integer_)
            piece_index = static_cast<IntegerData *>(v)->get_data();
        if (auto *v = dict->get_val("total_size"); v && v->type_ == DataType::integer_)
            total_size = static_cast<IntegerData *>(v)->get_data();

        delete parsed;

        if (msg_type != 1 || piece_index < 0)
            return false;

        if (total_size > 0 && metadata_size_ == 0)
        {
            set_metadata_size(static_cast<int32_t>(total_size));
        }

        size_t bencode_end = 0;
        int depth = 0;
        for (size_t i = 0; i < len; ++i)
        {
            if (data[i] == 'd' || data[i] == 'l')
            {
                depth++;
            }
            else if (data[i] == 'e')
            {
                if (depth == 0)
                    break;
                depth--;
                if (depth == 0)
                {
                    bencode_end = i + 1;
                    break;
                }
            }
            else if (std::isdigit(static_cast<unsigned char>(data[i])))
            {
                size_t j = i;
                while (j < len && std::isdigit(static_cast<unsigned char>(data[j])))
                    j++;
                if (j >= len || data[j] != ':')
                    break;
                j++;
                size_t str_len = 0;
                try
                {
                    str_len = std::stoll(std::string(reinterpret_cast<const char *>(data + i), j - i - 1));
                }
                catch (...)
                {
                    break;
                }
                j += str_len;
                i = j - 1;
                continue;
            }
            else if (data[i] == 'i')
            {
                size_t j = i + 1;
                while (j < len && data[j] != 'e')
                    j++;
                i = j;
                continue;
            }
        }

        if (bencode_end == 0 || bencode_end >= len)
            return false;

        const uint8_t *raw_metadata = data + bencode_end;
        size_t raw_len = len - bencode_end;

        int32_t piece_offset = static_cast<int32_t>(piece_index) * METADATA_PIECE_SIZE;
        if (piece_offset + static_cast<int32_t>(raw_len) > metadata_size_)
            return false;

        if (static_cast<int32_t>(piece_index) < metadata_piece_count())
        {
            received_pieces_[static_cast<int32_t>(piece_index)].assign(raw_metadata, raw_metadata + raw_len);
        }

        auto it = peer_states_.find(peer_key);
        if (it != peer_states_.end())
        {
            it->second.requested_pieces.erase(static_cast<int32_t>(piece_index));
        }

        try_assemble_metadata();
        return true;
    }

    bool MetadataManager::handle_metadata_request(PeerConnection *peer,
                                                    const std::string &peer_key,
                                                    const uint8_t *data,
                                                    size_t len)
    {
        (void)peer_key;

        std::string str(reinterpret_cast<const char *>(data), len);
        auto *parsed = BencodingDataConverter::parse(str);
        if (!parsed || parsed->type_ != DataType::dictionary_)
        {
            delete parsed;
            return false;
        }

        auto *dict = static_cast<DicttionaryData *>(parsed);
        int64_t piece_index = -1;
        if (auto *v = dict->get_val("piece"); v && v->type_ == DataType::integer_)
            piece_index = static_cast<IntegerData *>(v)->get_data();

        delete parsed;

        if (piece_index < 0)
            return false;

        if (!metadata_complete_)
        {
            std::string reject;
            reject += "d";
            reject += "8:msg_type";
            reject += "i2e";
            reject += "5:piece";
            reject += "i";
            reject += std::to_string(piece_index);
            reject += "e";
            reject += "e";

            peer->send_extended(
                static_cast<uint8_t>(our_ut_metadata_ext_id_),
                reinterpret_cast<const uint8_t *>(reject.data()),
                reject.size());
            return true;
        }

        if (piece_index >= metadata_piece_count())
            return false;

        int32_t offset = static_cast<int32_t>(piece_index) * METADATA_PIECE_SIZE;
        int32_t piece_len = std::min(METADATA_PIECE_SIZE, metadata_size_ - offset);

        std::string response;
        response += "d";
        response += "8:msg_type";
        response += "i1e";
        response += "5:piece";
        response += "i";
        response += std::to_string(piece_index);
        response += "e";
        response += "10:total_size";
        response += "i";
        response += std::to_string(metadata_size_);
        response += "e";
        response += "e";

        std::vector<uint8_t> payload;
        payload.assign(response.begin(), response.end());
        payload.insert(payload.end(), metadata_.data() + offset, metadata_.data() + offset + piece_len);

        auto msg = PeerMessage::extended(
            static_cast<uint8_t>(our_ut_metadata_ext_id_),
            payload.data(), payload.size());
        peer->send_extended(msg.extended_id(), msg.extended_payload(), msg.extended_payload_size());

        return true;
    }

    bool MetadataManager::handle_metadata_reject(const std::string &peer_key,
                                                  const uint8_t *data,
                                                  size_t len)
    {
        (void)data;
        (void)len;

        auto it = peer_states_.find(peer_key);
        if (it != peer_states_.end())
        {
            it->second.supports_metadata = false;
            it->second.requested_pieces.clear();
        }
        return true;
    }

    void MetadataManager::request_metadata_from_peer(PeerConnection *peer,
                                                      const std::string &peer_key)
    {
        if (metadata_complete_)
            return;

        auto it = peer_states_.find(peer_key);
        if (it == peer_states_.end() || !it->second.supports_metadata)
            return;

        if (metadata_size_ <= 0)
            return;

        int32_t count = metadata_piece_count();
        for (int32_t i = 0; i < count; ++i)
        {
            if (received_pieces_.count(i))
                continue;
            if (it->second.requested_pieces.count(i))
                continue;

            std::string request;
            request += "d";
            request += "8:msg_type";
            request += "i0e";
            request += "5:piece";
            request += "i";
            request += std::to_string(i);
            request += "e";
            request += "e";

            peer->send_extended(
                static_cast<uint8_t>(it->second.ut_metadata_msg_id),
                reinterpret_cast<const uint8_t *>(request.data()),
                request.size());

            it->second.requested_pieces.insert(i);

            if (it->second.requested_pieces.size() >= 3)
                break;
        }
    }

    void MetadataManager::try_assemble_metadata()
    {
        if (metadata_complete_ || metadata_size_ <= 0)
            return;

        int32_t count = metadata_piece_count();
        for (int32_t i = 0; i < count; ++i)
        {
            if (received_pieces_.find(i) == received_pieces_.end())
                return;
        }

        size_t offset = 0;
        for (int32_t i = 0; i < count; ++i)
        {
            const auto &piece = received_pieces_[i];
            size_t copy_len = std::min(piece.size(), metadata_.size() - offset);
            std::memcpy(metadata_.data() + offset, piece.data(), copy_len);
            offset += copy_len;
        }

        auto computed_hash = sha1_hash(
            reinterpret_cast<const unsigned char *>(metadata_.data()),
            metadata_.size());

        if (computed_hash.size() == info_hash_.size() &&
            std::memcmp(computed_hash.data(), info_hash_.data(), info_hash_.size()) == 0)
        {
            metadata_complete_ = true;
            if (metadata_complete_cb_)
            {
                metadata_complete_cb_(metadata_);
            }
        }
        else
        {
            received_pieces_.clear();
        }
    }

    bool MetadataManager::peer_supports_metadata(const std::string &peer_key) const
    {
        auto it = peer_states_.find(peer_key);
        return it != peer_states_.end() && it->second.supports_metadata;
    }

} // namespace yuan::net::bit_torrent
