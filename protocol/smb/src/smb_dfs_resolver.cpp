#include "smb_dfs_resolver.h"
#include <algorithm>

namespace yuan::net::smb
{
    void SmbDfsResolver::add_referral(const DfsReferral & referral)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        referrals_[referral.dfs_path] = referral;
    }

    void SmbDfsResolver::remove_referral(const std::string & dfs_path)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        referrals_.erase(dfs_path);
    }

    std::optional<DfsReferral> SmbDfsResolver::resolve(const std::string & path) const
    {
        std::lock_guard<std::mutex> lock(mutex_);

        auto it = referrals_.find(path);
        if (it != referrals_.end()) {
            return it->second;
        }

        size_t best_len = 0;
        std::optional<DfsReferral> best;

        for (const auto & [
                              key,
                              ref
                          ] : referrals_) {
            if (path.size() > key.size() && path.substr(0, key.size()) == key) {
                if (key.size() > best_len) {
                    best_len = key.size();
                    best = ref;
                }
            }
        }

        return best;
    }

    std::vector<DfsReferral> SmbDfsResolver::list_referrals() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<DfsReferral> result;
        result.reserve(referrals_.size());
        for (const auto & [
                              path,
                              ref
                          ] : referrals_) {
            result.push_back(ref);
        }
        return result;
    }
}
