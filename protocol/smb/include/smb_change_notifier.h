#ifndef __NET_SMB_SMB_CHANGE_NOTIFIER_H__
#define __NET_SMB_SMB_CHANGE_NOTIFIER_H__

#include "protocol/smb2_constants.h"
#include "protocol/smb2_structures.h"
#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <unordered_map>

namespace yuan::net::smb
{
    class SmbChangeNotifier
    {
    public:
        using NotifyCallback = std::function<void(const FileId &file_id, uint32_t action)>;

        uint64_t register_watch(const FileId &file_id, uint32_t completion_filter, NotifyCallback callback);
        void unregister_watch(uint64_t watch_id);
        void notify_change(const FileId &file_id, uint32_t action);
        void cancel_session_watches(uint64_t session_id);

    private:
        struct WatchEntry
        {
            uint64_t watch_id = 0;
            FileId file_id;
            uint32_t completion_filter = 0;
            uint64_t session_id = 0;
            NotifyCallback callback;
        };

        mutable std::mutex mutex_;
        std::unordered_map<uint64_t, WatchEntry> watches_;
        std::atomic<uint64_t> next_watch_id_{ 1 };
    };
}
#endif
