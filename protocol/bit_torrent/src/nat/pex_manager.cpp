#include "nat/pex_manager.h"
#include "nat/nat_config.h"
#include "structure/bencoding.h"
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#endif
#include <cstring>
#include <algorithm>

namespace yuan::net::bit_torrent
{

PexManager::PexManager()
    : next_ext_id_(1)
{
}

PexManager::~PexManager() {}

void PexManager::init(const std::vector<uint8_t> &info_hash, const NatConfig &config)
{
    info_hash_ = info_hash;
    config_ = config;

    // Assign our extension message ID for ut_pex
    // We use ID 1 (first available after ext handshake which is always 0)
    our_ut_pex_id_str_ = std::to_string(next_ext_id_++);
}

void PexManager::on_peer_handshake(const uint8_t *reserved_bytes)
{
    // Check if the peer supports extensions (BEP 10)
    // reserved[5] bit 4 (0x10) indicates extension protocol support
    if (reserved_bytes[5] & 0x10)
    {
        // Mark this peer as supporting extensions
        // Note: we don't know the peer_key yet, it will be set via register_peer
    }
}

bool PexManager::on_extended_message(const std::string &peer_key,
                                      uint8_t ext_id,
                                      const uint8_t *payload, size_t len)
{
    if (ext_id == 0)
    {
        // Extension handshake
        return parse_ext_handshake(peer_key, payload, len);
    }

    // Check if this is a ut_pex message from this peer
    auto it = peer_states_.find(peer_key);
    if (it != peer_states_.end() && it->second.supports_pex)
    {
        if (ext_id == static_cast<uint8_t>(it->second.ut_pex_msg_id))
        {
            return parse_pex_message(peer_key, payload, len);
        }
    }

    return false;
}

std::vector<uint8_t> PexManager::build_ext_handshake() const
{
    // Build extension handshake bencoded dict:
    // { "m": { "ut_pex": <our_ext_id> }, "v": "YZ0001", "reqq": 50 }
    //
    // Manual bencode for simplicity (avoid allocating BaseData objects):

    std::string dict;
    // "m" key
    dict += "1:m";  // key "m"

    // "m" value: d1:ut_pexi<id>ee
    dict += "d";
    dict += "6:ut_pex";
    dict += "i";
    dict += our_ut_pex_id_str_;
    dict += "e";
    dict += "e";

    // "v" key
    dict += "1:v";
    // "v" value
    std::string version = "YZ0001";
    dict += std::to_string(version.size()) + ":" + version;

    // "reqq" key
    dict += "4:reqq";
    // "reqq" value
    dict += "i50e";

    return std::vector<uint8_t>(dict.begin(), dict.end());
}

std::vector<uint8_t> PexManager::build_pex_message(const std::string &peer_key)
{
    auto it = peer_states_.find(peer_key);
    if (it == peer_states_.end() || !it->second.supports_pex)
        return {};

    PeerExtensionState &state = it->second;

    // Build the "added" compact peer list (peers we haven't sent to this peer yet)
    std::vector<PexPeerInfo> to_send;
    for (const auto &p : added_since_last_flush_)
    {
        std::string pkey = p.ip + ":" + std::to_string(p.port);
        if (state.sent_peers.find(pkey) == state.sent_peers.end() && pkey != peer_key)
        {
            to_send.push_back(p);
            state.sent_peers.insert(pkey);
        }
    }

    // Also include recently dropped peers
    std::vector<uint8_t> dropped_compact;
    for (const auto &dk : state.dropped_peers)
    {
        // Parse ip:port back to compact format
        size_t colon = dk.find(':');
        if (colon != std::string::npos)
        {
            std::string ip = dk.substr(0, colon);
            uint16_t port = static_cast<uint16_t>(std::atoi(dk.substr(colon + 1).c_str()));

            // Compact format: 4-byte IP + 2-byte port (network order)
            uint8_t compact[6];
            inet_pton(AF_INET, ip.c_str(), compact);
            compact[4] = (port >> 8) & 0xFF;
            compact[5] = port & 0xFF;
            dropped_compact.insert(dropped_compact.end(), compact, compact + 6);
        }
    }
    state.dropped_peers.clear();

    if (to_send.empty() && dropped_compact.empty())
        return {};

    // Build bencoded dict: d5:added<compact>5:dropped<compact>e
    std::vector<uint8_t> added_compact = build_compact_peers(to_send);

    std::string msg;
    msg += "d";

    if (!added_compact.empty())
    {
        msg += "5:added";
        msg += std::to_string(added_compact.size()) + ":";
        // Append binary data
        msg.append(reinterpret_cast<const char *>(added_compact.data()), added_compact.size());
    }

    if (!dropped_compact.empty())
    {
        msg += "7:dropped";
        msg += std::to_string(dropped_compact.size()) + ":";
        msg.append(reinterpret_cast<const char *>(dropped_compact.data()), dropped_compact.size());
    }

    msg += "e";

    return std::vector<uint8_t>(msg.begin(), msg.end());
}

void PexManager::add_peer(const std::string &ip, uint16_t port)
{
    std::string key = ip + ":" + std::to_string(port);

    if (known_peers_.find(key) != known_peers_.end())
        return;

    // Don't add too many
    if (known_peers_.size() >= config_.pex_max_peers)
        return;

    PexPeerInfo info;
    info.ip = ip;
    info.port = port;
    known_peers_[key] = info;

    added_since_last_flush_.push_back(info);

    if (new_peer_cb_)
    {
        std::vector<PexPeerInfo> new_peers = {info};
        new_peer_cb_(new_peers);
    }
}

void PexManager::remove_peer(const std::string &peer_key)
{
    known_peers_.erase(peer_key);
    peer_states_.erase(peer_key);

    // Add to all peers' dropped lists
    for (auto &p : peer_states_)
    {
        p.second.dropped_peers.insert(peer_key);
    }
}

std::vector<PexPeerInfo> PexManager::get_all_peers() const
{
    std::vector<PexPeerInfo> result;
    for (const auto &pair : known_peers_)
    {
        result.push_back(pair.second);
    }
    return result;
}

bool PexManager::peer_supports_pex(const std::string &peer_key) const
{
    auto it = peer_states_.find(peer_key);
    return it != peer_states_.end() && it->second.supports_pex;
}

int PexManager::get_peer_pex_ext_id(const std::string &peer_key) const
{
    auto it = peer_states_.find(peer_key);
    if (it != peer_states_.end() && it->second.supports_pex)
        return it->second.ut_pex_msg_id;
    return 0;
}

bool PexManager::parse_ext_handshake(const std::string &peer_key,
                                      const uint8_t *data, size_t len)
{
    // Parse the extension handshake bencoded dict
    // Expected format: { "m": { "ut_pex": <msg_id>, ... }, ... }

    std::string str(reinterpret_cast<const char *>(data), len);
    auto *parsed = BencodingDataConverter::parse(str);
    if (!parsed || parsed->type_ != DataType::dictionary_)
    {
        delete parsed;
        return false;
    }

    auto *dict = static_cast<DicttionaryData *>(parsed);

    PeerExtensionState state;

    // Check for "m" key
    auto *m = dict->get_val("m");
    if (m && m->type_ == DataType::dictionary_)
    {
        auto *m_dict = static_cast<DicttionaryData *>(m);
        state.supports_extensions = true;

        // Check for "ut_pex" in the m dict
        auto *ut_pex = m_dict->get_val("ut_pex");
        if (ut_pex && ut_pex->type_ == DataType::integer_)
        {
            state.supports_pex = true;
            state.ut_pex_msg_id = static_cast<IntegerData *>(ut_pex)->get_data();
        }
    }

    // We also need to create the peer state entry if it doesn't exist
    peer_states_[peer_key] = state;

    delete parsed;
    return state.supports_extensions;
}

bool PexManager::parse_pex_message(const std::string &peer_key,
                                    const uint8_t *data, size_t len)
{
    // Parse bencoded dict with "added" and optionally "dropped" compact peer lists
    std::string str(reinterpret_cast<const char *>(data), len);
    auto *parsed = BencodingDataConverter::parse(str);
    if (!parsed || parsed->type_ != DataType::dictionary_)
    {
        delete parsed;
        return false;
    }

    auto *dict = static_cast<DicttionaryData *>(parsed);

    // Process "added" peers
    auto *added = dict->get_val("added");
    if (added && added->type_ == DataType::string_)
    {
        const std::string &added_str = static_cast<StringData *>(added)->get_data();
        auto peers = parse_compact_peers(
            reinterpret_cast<const uint8_t *>(added_str.data()), added_str.size());

        // Add discovered peers (if not already known)
        std::vector<PexPeerInfo> new_peers;
        for (const auto &p : peers)
        {
            std::string key = p.ip + ":" + std::to_string(p.port);
            if (known_peers_.find(key) == known_peers_.end())
            {
                if (known_peers_.size() < config_.pex_max_peers)
                {
                    known_peers_[key] = p;
                    added_since_last_flush_.push_back(p);
                    new_peers.push_back(p);
                }
            }
        }

        if (new_peer_cb_ && !new_peers.empty())
        {
            new_peer_cb_(new_peers);
        }
    }

    // Process "dropped" peers
    auto *dropped = dict->get_val("dropped");
    if (dropped && dropped->type_ == DataType::string_)
    {
        const std::string &dropped_str = static_cast<StringData *>(dropped)->get_data();
        auto peers = parse_compact_peers(
            reinterpret_cast<const uint8_t *>(dropped_str.data()), dropped_str.size());

        for (const auto &p : peers)
        {
            std::string key = p.ip + ":" + std::to_string(p.port);
            known_peers_.erase(key);
        }
    }

    delete parsed;
    return true;
}

std::vector<PexPeerInfo> PexManager::parse_compact_peers(const uint8_t *data, size_t len)
{
    std::vector<PexPeerInfo> result;
    // Each peer is 6 bytes: 4-byte IP + 2-byte port
    size_t count = len / 6;
    result.reserve(count);

    for (size_t i = 0; i < count; i++)
    {
        const uint8_t *p = data + i * 6;

        char ip_str[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, p, ip_str, sizeof(ip_str));

        uint16_t port = (static_cast<uint16_t>(p[4]) << 8) | p[5];

        PexPeerInfo info;
        info.ip = ip_str;
        info.port = port;
        result.push_back(info);
    }

    return result;
}

std::vector<uint8_t> PexManager::build_compact_peers(const std::vector<PexPeerInfo> &peers)
{
    std::vector<uint8_t> result;
    result.reserve(peers.size() * 6);

    for (const auto &p : peers)
    {
        uint8_t compact[6];
        inet_pton(AF_INET, p.ip.c_str(), compact);
        compact[4] = (p.port >> 8) & 0xFF;
        compact[5] = p.port & 0xFF;
        result.insert(result.end(), compact, compact + 6);
    }

    return result;
}

} // namespace yuan::net::bit_torrent
