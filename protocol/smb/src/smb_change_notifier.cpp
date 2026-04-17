#include "smb_change_notifier.h"
#include <algorithm>

namespace yuan::net::smb
{
    uint64_t SmbChangeNotifier::register_watch(const FileId & file_id, uint32_t completion_filter, NotifyCallback callback)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        uint64_t wid = next_watch_id_.fetch_add(1);
        WatchEntry entry;
        entry.watch_id = wid;
        entry.file_id = file_id;
        entry.completion_filter = completion_filter;
        entry.callback = std::move(callback);
        watches_[wid] = std::move(entry);
        return wid;
    }

    void SmbChangeNotifier::unregister_watch(uint64_t watch_id)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        watches_.erase(watch_id);
    }

    void SmbChangeNotifier::notify_change(const FileId & file_id, uint32_t action)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto & [
                        id,
                        entry
                    ] : watches_) {
            if (entry.file_id.persistent == file_id.persistent) {
                if (entry.callback) {
                    entry.callback(file_id, action);
                }
            }
        }
    }

    void SmbChangeNotifier::cancel_session_watches(uint64_t session_id)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto it = watches_.begin(); it != watches_.end();) {
            if (it->second.session_id == session_id) {
                it = watches_.erase(it);
            } else {
                ++it;
            }
        }
    }
}
