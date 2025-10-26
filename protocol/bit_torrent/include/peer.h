#ifndef __PEER_H__
#define __PEER_H__
#include "net/connection/connection.h"

namespace yuan::bit_torrent
{
    class BitTorrentPeer
    {
    public:
        BitTorrentPeer();
        ~BitTorrentPeer();

    public:
        void connect();
        void disconnect();

        int handshake();

        int do_handshake();

    private:
        int peer_id_;
        net::Connection *conn_;
    };
}

#endif // __PEER_H__
