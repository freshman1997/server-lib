#ifndef __BIT_TORRENT_SESSION_PEER_SESSION_H__
#define __BIT_TORRENT_SESSION_PEER_SESSION_H__

#include "torrent_meta.h"
#include "nat/nat_manager.h"
#include "peer_wire/peer_connection.h"
#include "net/runtime/network_runtime.h"
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace yuan::net::bit_torrent
{

    struct PeerSessionConfig
    {
        net::NetworkRuntime *runtime_ = nullptr;
        NatManager *nat_manager_ = nullptr;
        const TorrentMeta *meta_ = nullptr;
        const std::string *peer_id_ = nullptr;
        const std::vector<bool> *pieces_have_ = nullptr;
        int32_t max_peers_ = 50;
        bool allow_loopback_peers_ = false;
        PieceDataHandler piece_data_handler_;
        PieceRequestHandler piece_request_handler_;
        PieceServedHandler piece_served_handler_;
        std::function<void(PeerConnection *)> peer_ready_handler_;
        std::function<void(PeerConnection *)> peer_unchoke_handler_;
        std::function<void(PeerConnection *)> peer_piece_availability_handler_;
        std::function<void(PeerConnection *, uint32_t, uint32_t, uint32_t)> peer_reject_handler_;
        std::function<void(const std::vector<PieceBlockRequest> &)> peer_lost_handler_;
    };

    class PeerSession
    {
    public:
        using PeerReadyHandler = std::function<void(PeerConnection *)>;
        using PeerLostHandler = std::function<void(const std::vector<PieceBlockRequest> &)>;

    public:
        void configure(PeerSessionConfig config);
        void bind_nat_runtime();
        void set_runtime(net::NetworkRuntime *runtime);
        void set_nat_manager(NatManager *nat_manager);
        void set_context(const TorrentMeta *meta,
                         const std::string *peer_id,
                         const std::vector<bool> *pieces_have);
        void set_max_peers(int32_t max_peers)
        {
            max_peers_ = max_peers;
        }
        void set_piece_data_handler(PieceDataHandler handler)
        {
            piece_data_handler_ = std::move(handler);
        }
        void set_piece_request_handler(PieceRequestHandler handler)
        {
            piece_request_handler_ = std::move(handler);
        }
        void set_piece_served_handler(PieceServedHandler handler)
        {
            piece_served_handler_ = std::move(handler);
        }
        void set_peer_ready_handler(PeerReadyHandler handler)
        {
            peer_ready_handler_ = std::move(handler);
        }
        void set_peer_unchoke_handler(PeerReadyHandler handler)
        {
            peer_unchoke_handler_ = std::move(handler);
        }
        void set_peer_piece_availability_handler(PeerReadyHandler handler)
        {
            peer_piece_availability_handler_ = std::move(handler);
        }
        void set_peer_reject_handler(std::function<void(PeerConnection *, uint32_t, uint32_t, uint32_t)> handler)
        {
            peer_reject_handler_ = std::move(handler);
        }
        void set_peer_lost_handler(PeerLostHandler handler)
        {
            peer_lost_handler_ = std::move(handler);
        }

        void connect_peers(const std::vector<PeerAddress> &peer_list);
        void on_inbound_peer(std::shared_ptr<PeerConnection> peer);
        void disconnect_all_peers();
        void broadcast_have(uint32_t piece_index);
        std::vector<std::shared_ptr<PeerConnection> > get_active_peers() const;

        int32_t get_peer_count() const;
        int32_t get_active_peer_count() const;

    private:
        std::string make_key(const std::string &ip, uint16_t port) const;
        void attach_peer(const std::shared_ptr<PeerConnection> &peer, const std::string &key);
        void on_peer_state_changed(PeerConnection *peer);
        void on_peer_connected(PeerConnection &peer);
        void remove_peer(PeerConnection &peer);
        void pump_peer_queue();

    private:
        net::NetworkRuntime *runtime_ = nullptr;
        NatManager *nat_manager_ = nullptr;

        const TorrentMeta *meta_ = nullptr;
        const std::string *peer_id_ = nullptr;
        const std::vector<bool> *pieces_have_ = nullptr;

        int32_t max_peers_ = 50;
        bool allow_loopback_peers_ = false;
        PieceDataHandler piece_data_handler_;
        PieceRequestHandler piece_request_handler_;
        PieceServedHandler piece_served_handler_;
        PeerReadyHandler peer_ready_handler_;
        PeerReadyHandler peer_unchoke_handler_;
        PeerReadyHandler peer_piece_availability_handler_;
        std::function<void(PeerConnection *, uint32_t, uint32_t, uint32_t)> peer_reject_handler_;
        PeerLostHandler peer_lost_handler_;

        mutable std::mutex peers_mutex_;
        std::unordered_map<std::string, std::shared_ptr<PeerConnection> > peers_;
        std::deque<PeerAddress> pending_peers_;
        std::unordered_set<std::string> pending_peer_keys_;
        std::unordered_set<std::string> utp_attempted_keys_;
        std::unordered_map<std::string, uint64_t> peer_retry_after_ms_;
    };

} // namespace yuan::net::bit_torrent

#endif // __BIT_TORRENT_SESSION_PEER_SESSION_H__
