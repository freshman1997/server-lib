#ifndef __BIT_TORRENT_PEER_WIRE_MESSAGE_H__
#define __BIT_TORRENT_PEER_WIRE_MESSAGE_H__

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace yuan::net::bit_torrent
{

// Peer Wire Protocol message IDs (BEP 3)
enum class PeerMessageId : uint8_t
{
    choke = 0,
    unchoke = 1,
    interested = 2,
    not_interested = 3,
    have = 4,
    bitfield = 5,
    request = 6,
    piece = 7,
    cancel = 8,
    // DHT (BEP 5)
    port = 9,
    // Fast extension (BEP 6)
    suggest_piece = 13,
    have_all = 14,
    have_none = 15,
    reject_request = 16,
    allowed_fast = 17,
    // Extension protocol (BEP 10)
    extended = 20,
};

// Handshake: <pstrlen><pstr><reserved><info_hash><peer_id>
struct HandshakeMessage
{
    static constexpr size_t PROTOCOL_STR_LEN = 19;
    static constexpr const char *PROTOCOL_STR = "BitTorrent protocol";
    static constexpr size_t HANDSHAKE_SIZE = 68; // 1 + 19 + 8 + 20 + 20

    uint8_t protocol_len_ = PROTOCOL_STR_LEN;
    char protocol_str_[PROTOCOL_STR_LEN + 1] = {};
    uint8_t reserved_[8] = {};
    uint8_t info_hash_[20] = {};
    uint8_t peer_id_[20] = {};

    HandshakeMessage()
    {
        std::memset(protocol_str_, 0, sizeof(protocol_str_));
        std::memcpy(protocol_str_, PROTOCOL_STR, PROTOCOL_STR_LEN);
        // Set DHT and Fast extension bits in reserved
        reserved_[7] |= 0x01; // DHT support (BEP 5)
        reserved_[5] |= 0x10; // Fast extension (BEP 6)
    }

    void set_info_hash(const std::vector<uint8_t> &hash)
    {
        std::memcpy(info_hash_, hash.data(), 20);
    }

    void set_info_hash(const uint8_t *hash)
    {
        std::memcpy(info_hash_, hash, 20);
    }

    void set_peer_id(const std::string &peer_id)
    {
        std::memcpy(peer_id_, peer_id.data(), std::min(peer_id.size(), size_t(20)));
    }

    std::vector<uint8_t> serialize() const
    {
        std::vector<uint8_t> buf(HANDSHAKE_SIZE);
        buf[0] = protocol_len_;
        std::memcpy(buf.data() + 1, protocol_str_, PROTOCOL_STR_LEN);
        std::memcpy(buf.data() + 20, reserved_, 8);
        std::memcpy(buf.data() + 28, info_hash_, 20);
        std::memcpy(buf.data() + 48, peer_id_, 20);
        return buf;
    }

    // Returns true if parsed successfully. Expects exactly 68 bytes.
    bool deserialize(const uint8_t *data, size_t len)
    {
        if (len < HANDSHAKE_SIZE) return false;
        if (data[0] != PROTOCOL_STR_LEN) return false;
        if (std::memcmp(data + 1, PROTOCOL_STR, PROTOCOL_STR_LEN) != 0) return false;

        std::memcpy(reserved_, data + 20, 8);
        std::memcpy(info_hash_, data + 28, 20);
        std::memcpy(peer_id_, data + 48, 20);
        return true;
    }

    bool supports_extension() const { return (reserved_[5] & 0x10) != 0; }
    bool supports_dht() const { return (reserved_[7] & 0x01) != 0; }
    bool supports_fast() const { return (reserved_[5] & 0x10) != 0; }
};

// Generic peer message: <length prefix><message ID><payload>
struct PeerMessage
{
    uint32_t length_ = 0;  // includes 1 byte for id (0 for keep-alive)
    PeerMessageId id_ = static_cast<PeerMessageId>(0);
    std::vector<uint8_t> payload_;

    bool is_keepalive() const { return length_ == 0; }

    // Serialize into buffer, returns bytes written
    size_t serialize(uint8_t *buf, size_t buf_len) const
    {
        uint32_t net_len = length_;
        if (buf_len < 4 + length_) return 0;

        // big-endian length
        buf[0] = (net_len >> 24) & 0xFF;
        buf[1] = (net_len >> 16) & 0xFF;
        buf[2] = (net_len >> 8) & 0xFF;
        buf[3] = net_len & 0xFF;

        if (length_ > 0)
        {
            buf[4] = static_cast<uint8_t>(id_);
            if (!payload_.empty())
                std::memcpy(buf + 5, payload_.data(), payload_.size());
        }

        return 4 + length_;
    }

    std::vector<uint8_t> serialize() const
    {
        std::vector<uint8_t> buf(4 + length_);
        serialize(buf.data(), buf.size());
        return buf;
    }

    // Factory methods
    static PeerMessage keepalive()
    {
        PeerMessage msg;
        msg.length_ = 0;
        return msg;
    }

    static PeerMessage choke()
    {
        PeerMessage msg;
        msg.length_ = 1;
        msg.id_ = PeerMessageId::choke;
        return msg;
    }

    static PeerMessage unchoke()
    {
        PeerMessage msg;
        msg.length_ = 1;
        msg.id_ = PeerMessageId::unchoke;
        return msg;
    }

    static PeerMessage interested()
    {
        PeerMessage msg;
        msg.length_ = 1;
        msg.id_ = PeerMessageId::interested;
        return msg;
    }

    static PeerMessage not_interested()
    {
        PeerMessage msg;
        msg.length_ = 1;
        msg.id_ = PeerMessageId::not_interested;
        return msg;
    }

    static PeerMessage have(uint32_t piece_index)
    {
        PeerMessage msg;
        msg.length_ = 5;
        msg.id_ = PeerMessageId::have;
        uint32_t net_idx = piece_index;
        uint8_t payload[4];
        payload[0] = (net_idx >> 24) & 0xFF;
        payload[1] = (net_idx >> 16) & 0xFF;
        payload[2] = (net_idx >> 8) & 0xFF;
        payload[3] = net_idx & 0xFF;
        msg.payload_.assign(payload, payload + 4);
        return msg;
    }

    static PeerMessage bitfield(const std::vector<uint8_t> &bits)
    {
        PeerMessage msg;
        msg.length_ = 1 + static_cast<uint32_t>(bits.size());
        msg.id_ = PeerMessageId::bitfield;
        msg.payload_ = bits;
        return msg;
    }

    static PeerMessage request(uint32_t piece_index, uint32_t offset, uint32_t length)
    {
        PeerMessage msg;
        msg.length_ = 13;
        msg.id_ = PeerMessageId::request;
        uint8_t payload[12];
        // piece index (big-endian)
        payload[0] = (piece_index >> 24) & 0xFF;
        payload[1] = (piece_index >> 16) & 0xFF;
        payload[2] = (piece_index >> 8) & 0xFF;
        payload[3] = piece_index & 0xFF;
        // offset (big-endian)
        payload[4] = (offset >> 24) & 0xFF;
        payload[5] = (offset >> 16) & 0xFF;
        payload[6] = (offset >> 8) & 0xFF;
        payload[7] = offset & 0xFF;
        // length (big-endian)
        payload[8] = (length >> 24) & 0xFF;
        payload[9] = (length >> 16) & 0xFF;
        payload[10] = (length >> 8) & 0xFF;
        payload[11] = length & 0xFF;
        msg.payload_.assign(payload, payload + 12);
        return msg;
    }

    static PeerMessage piece(uint32_t piece_index, uint32_t offset, const uint8_t *data, uint32_t length)
    {
        PeerMessage msg;
        msg.length_ = 9 + length;
        msg.id_ = PeerMessageId::piece;
        uint8_t header[8];
        header[0] = (piece_index >> 24) & 0xFF;
        header[1] = (piece_index >> 16) & 0xFF;
        header[2] = (piece_index >> 8) & 0xFF;
        header[3] = piece_index & 0xFF;
        header[4] = (offset >> 24) & 0xFF;
        header[5] = (offset >> 16) & 0xFF;
        header[6] = (offset >> 8) & 0xFF;
        header[7] = offset & 0xFF;
        msg.payload_.assign(header, header + 8);
        msg.payload_.insert(msg.payload_.end(), data, data + length);
        return msg;
    }

    static PeerMessage cancel(uint32_t piece_index, uint32_t offset, uint32_t length)
    {
        // cancel has same format as request
        PeerMessage msg;
        msg.length_ = 13;
        msg.id_ = PeerMessageId::cancel;
        uint8_t payload[12];
        payload[0] = (piece_index >> 24) & 0xFF;
        payload[1] = (piece_index >> 16) & 0xFF;
        payload[2] = (piece_index >> 8) & 0xFF;
        payload[3] = piece_index & 0xFF;
        payload[4] = (offset >> 24) & 0xFF;
        payload[5] = (offset >> 16) & 0xFF;
        payload[6] = (offset >> 8) & 0xFF;
        payload[7] = offset & 0xFF;
        payload[8] = (length >> 24) & 0xFF;
        payload[9] = (length >> 16) & 0xFF;
        payload[10] = (length >> 8) & 0xFF;
        payload[11] = length & 0xFF;
        msg.payload_.assign(payload, payload + 12);
        return msg;
    }

    static PeerMessage port(uint16_t listen_port)
    {
        PeerMessage msg;
        msg.length_ = 3;
        msg.id_ = PeerMessageId::port;
        msg.payload_.push_back((listen_port >> 8) & 0xFF);
        msg.payload_.push_back(listen_port & 0xFF);
        return msg;
    }

    // Parse a message from raw bytes. Returns bytes consumed, 0 if incomplete.
    static int parse(const uint8_t *data, size_t len, PeerMessage &out)
    {
        if (len < 4) return 0;

        uint32_t msg_len = (static_cast<uint32_t>(data[0]) << 24) |
                           (static_cast<uint32_t>(data[1]) << 16) |
                           (static_cast<uint32_t>(data[2]) << 8) |
                           static_cast<uint32_t>(data[3]);

        if (msg_len == 0)
        {
            out = keepalive();
            return 4;
        }

        if (len < 4 + msg_len) return 0;

        out.length_ = msg_len;
        out.id_ = static_cast<PeerMessageId>(data[4]);
        if (msg_len > 1)
            out.payload_.assign(data + 5, data + 4 + msg_len);
        else
            out.payload_.clear();

        return 4 + msg_len;
    }

    // Accessors for specific message types
    uint32_t have_piece_index() const
    {
        if (id_ != PeerMessageId::have || payload_.size() < 4) return 0;
        return (static_cast<uint32_t>(payload_[0]) << 24) |
               (static_cast<uint32_t>(payload_[1]) << 16) |
               (static_cast<uint32_t>(payload_[2]) << 8) |
               static_cast<uint32_t>(payload_[3]);
    }

    uint32_t request_piece_index() const
    {
        if (payload_.size() < 4) return 0;
        return (static_cast<uint32_t>(payload_[0]) << 24) |
               (static_cast<uint32_t>(payload_[1]) << 16) |
               (static_cast<uint32_t>(payload_[2]) << 8) |
               static_cast<uint32_t>(payload_[3]);
    }

    uint32_t request_offset() const
    {
        if (payload_.size() < 8) return 0;
        return (static_cast<uint32_t>(payload_[4]) << 24) |
               (static_cast<uint32_t>(payload_[5]) << 16) |
               (static_cast<uint32_t>(payload_[6]) << 8) |
               static_cast<uint32_t>(payload_[7]);
    }

    uint32_t request_length() const
    {
        if (payload_.size() < 12) return 0;
        return (static_cast<uint32_t>(payload_[8]) << 24) |
               (static_cast<uint32_t>(payload_[9]) << 16) |
               (static_cast<uint32_t>(payload_[10]) << 8) |
               static_cast<uint32_t>(payload_[11]);
    }

    uint32_t piece_block_index() const
    {
        if (payload_.size() < 4) return 0;
        return (static_cast<uint32_t>(payload_[0]) << 24) |
               (static_cast<uint32_t>(payload_[1]) << 16) |
               (static_cast<uint32_t>(payload_[2]) << 8) |
               static_cast<uint32_t>(payload_[3]);
    }

    uint32_t piece_block_offset() const
    {
        if (payload_.size() < 8) return 0;
        return (static_cast<uint32_t>(payload_[4]) << 24) |
               (static_cast<uint32_t>(payload_[5]) << 16) |
               (static_cast<uint32_t>(payload_[6]) << 8) |
               static_cast<uint32_t>(payload_[7]);
    }

    const uint8_t *piece_block_data() const
    {
        if (payload_.size() < 9) return nullptr;
        return payload_.data() + 8;
    }

    uint32_t piece_block_size() const
    {
        if (payload_.size() < 9) return 0;
        return static_cast<uint32_t>(payload_.size()) - 8;
    }
};

// PeerConnection state machine
struct PeerState
{
    bool am_choking = true;
    bool am_interested = false;
    bool peer_choking = true;
    bool peer_interested = false;

    std::vector<bool> pieces;  // bitfield: which pieces the peer has

    bool has_piece(int32_t index) const
    {
        if (index < 0 || static_cast<size_t>(index) >= pieces.size()) return false;
        return pieces[index];
    }

    void set_have_piece(int32_t index, int32_t total_pieces)
    {
        if (index < 0) return;
        if (static_cast<size_t>(index) >= pieces.size())
            pieces.resize(index + 1, false);
        pieces[index] = true;
    }

    void set_bitfield(const std::vector<uint8_t> &bf, int32_t total_pieces)
    {
        pieces.assign(total_pieces, false);
        for (int32_t i = 0; i < total_pieces; i++)
        {
            int byte_idx = i / 8;
            int bit_idx = 7 - (i % 8);
            if (byte_idx < static_cast<int32_t>(bf.size()) && (bf[byte_idx] & (1 << bit_idx)))
                pieces[i] = true;
        }
    }

    std::vector<uint8_t> to_bitfield() const
    {
        if (pieces.empty()) return {};
        size_t byte_count = (pieces.size() + 7) / 8;
        std::vector<uint8_t> bf(byte_count, 0);
        for (size_t i = 0; i < pieces.size(); i++)
        {
            if (pieces[i])
                bf[i / 8] |= (1 << (7 - (i % 8)));
        }
        return bf;
    }
};

} // namespace yuan::net::bit_torrent

#endif // __BIT_TORRENT_PEER_WIRE_MESSAGE_H__
