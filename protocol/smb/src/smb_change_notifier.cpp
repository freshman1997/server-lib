#include "smb_change_notifier.h"
#include <algorithm>
#include <vector>

namespace yuan::net::smb
{
    namespace
    {
        bool matches_completion_filter(uint32_t completion_filter, uint32_t action)
        {
            if (completion_filter == 0) {
                return true;
            }
            return (completion_filter & action) != 0;
        }
    }

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
        std::vector<NotifyCallback> callbacks;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            callbacks.reserve(watches_.size());
            for (auto & [
                            id,
                            entry
                        ] : watches_) {
                (void)id;
                if (entry.file_id.persistent == file_id.persistent &&
                    matches_completion_filter(entry.completion_filter, action) &&
                    entry.callback) {
                    callbacks.push_back(entry.callback);
                }
            }
        }

        for (auto &callback : callbacks) {
            callback(file_id, action);
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
