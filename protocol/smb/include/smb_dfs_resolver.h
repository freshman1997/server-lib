#ifndef __NET_SMB_SMB_DFS_RESOLVER_H__
#define __NET_SMB_SMB_DFS_RESOLVER_H__

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace yuan::net::smb
{
    struct DfsReferral
    {
        std::string dfs_path;
        std::string target_server;
        std::string target_share;
        std::string target_path;
        uint32_t version = 4;
        uint32_t ttl = 300;
    };

    class SmbDfsResolver
    {
    public:
        void add_referral(const DfsReferral &referral);
        void remove_referral(const std::string &dfs_path);
        std::optional<DfsReferral> resolve(const std::string &path) const;
        std::vector<DfsReferral> list_referrals() const;

    private:
        mutable std::mutex mutex_;
        std::unordered_map<std::string, DfsReferral> referrals_;
    };
}
#endif
