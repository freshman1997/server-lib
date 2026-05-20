#ifndef __BIT_TORRENT_METADATA_MANAGER_H__
#define __BIT_TORRENT_METADATA_MANAGER_H__

// ut_metadata extension - BEP 9
//
// Allows peers to exchange the metadata (info dictionary) of a torrent.
// This is the core mechanism that makes magnet links work:
//   1. Client knows only the info_hash (from the magnet URI)
//   2. Connects to peers that support the extension protocol
//   3. Exchanges extension handshake to discover ut_metadata msg ID
//   4. Requests metadata pieces from peers
//   5. Reassembles the full info dict
//   6. Verifies SHA-1 matches the expected info_hash
//   7. Transitions to normal download mode
//
// Extension handshake includes:
//   { "m": { "ut_metadata": <id> }, "metadata_size": <size> }
//
// ut_metadata messages (bencoded dicts):
//   - request:  { "msg_type": 0, "piece": <index> }
//   - data:     { "msg_type": 1, "piece": <index>, "total_size": <size> } + <raw metadata bytes>
//   - reject:   { "msg_type": 2, "piece": <index> }

#include "torrent_meta.h"
#include "peer_wire/peer_wire_message.h"
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <string>
#include <cstdint>
#include <functional>

namespace yuan::net::bit_torrent
{

    class PeerConnection;

    class MetadataManager
    {
    public:
        MetadataManager();
        ~MetadataManager();

        void init(const std::vector<uint8_t> &info_hash);

        void set_metadata_size(int32_t size);

        bool on_extended_message(PeerConnection *peer,
                                 const std::string &peer_key,
                                 uint8_t ext_id,
                                 const uint8_t *payload,
                                 size_t len);

        std::vector<uint8_t> build_ext_handshake() const;

        bool has_metadata() const { return metadata_complete_; }

        const std::vector<uint8_t> &get_metadata() const { return metadata_; }

        void request_metadata_from_peer(PeerConnection *peer, const std::string &peer_key);

        using MetadataCompleteCallback = std::function<void(const std::vector<uint8_t> &metadata)>;
        void set_metadata_complete_callback(MetadataCompleteCallback cb) { metadata_complete_cb_ = std::move(cb); }

        bool peer_supports_metadata(const std::string &peer_key) const;
        int32_t metadata_size() const { return metadata_size_; }

    private:
        bool parse_ext_handshake(const std::string &peer_key,
                                 const uint8_t *data, size_t len);
        bool handle_metadata_data(const std::string &peer_key,
                                  const uint8_t *data, size_t len);
        bool handle_metadata_request(PeerConnection *peer,
                                     const std::string &peer_key,
                                     const uint8_t *data, size_t len);
        bool handle_metadata_reject(const std::string &peer_key,
                                    const uint8_t *data, size_t len);
        void try_assemble_metadata();

        static constexpr int32_t METADATA_PIECE_SIZE = 16 * 1024;

        int32_t metadata_piece_count() const
        {
            if (metadata_size_ <= 0)
                return 0;
            return (metadata_size_ + METADATA_PIECE_SIZE - 1) / METADATA_PIECE_SIZE;
        }

    private:
        std::vector<uint8_t> info_hash_;
        int32_t metadata_size_ = 0;
        bool metadata_complete_ = false;
        std::vector<uint8_t> metadata_;

        struct PeerMetadataState
        {
            bool supports_metadata = false;
            int ut_metadata_msg_id = 0;
            int32_t metadata_size = 0;
            std::unordered_set<int32_t> requested_pieces;
        };
        std::unordered_map<std::string, PeerMetadataState> peer_states_;

        int next_ext_id_ = 2;
        int our_ut_metadata_ext_id_ = 0;
        std::string our_ut_metadata_id_str_;

        std::unordered_map<int32_t, std::vector<uint8_t>> received_pieces_;

        MetadataCompleteCallback metadata_complete_cb_;
    };

} // namespace yuan::net::bit_torrent

#endif
