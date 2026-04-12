#ifndef __BIT_TORRENT_TRACKER_ANNOUNCE_EVENT_H__
#define __BIT_TORRENT_TRACKER_ANNOUNCE_EVENT_H__

#include <cstdint>

namespace yuan::net::bit_torrent
{

enum class TrackerAnnounceEvent : int32_t
{
    none = 0,
    completed,
    started,
    stopped,
};

} // namespace yuan::net::bit_torrent

#endif
