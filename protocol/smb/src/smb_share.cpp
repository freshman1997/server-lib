#include "smb_share.h"
#include "protocol/smb2_codec.h"
#include "base/owner_ptr.h"

#include <algorithm>
#include <cstring>
#include <filesystem>

namespace yuan::net::smb
{
    SmbShare::SmbShare(const SmbShareConfig & config)
        : name_(config.name), comment_(config.comment), type_(config.type), path_(config.path), share_flags_(config.share_flags), capabilities_(config.capabilities), max_uses_(config.max_uses)
    {
    }

    SmbShare::~SmbShare() = default;

    std::string SmbShare::resolve_path(const std::u16string & relative) const
    {
        return resolve_path(Smb2Codec::utf16le_to_utf8(relative));
    }

    std::string SmbShare::resolve_path(const std::string & relative) const
    {
        if (type_ == ShareType::PIPE) {
            return relative;
        }

        std::string normalized = relative;
        std::replace(normalized.begin(), normalized.end(), '\\', '/');

        while (!normalized.empty() && normalized[0] == '/') {
            normalized.erase(normalized.begin());
        }

        namespace fs = std::filesystem;
        fs::path root(path_);
        fs::path combined = root / normalized;
        fs::path canonical = fs::weakly_canonical(combined);
        fs::path canonical_root = fs::weakly_canonical(root);

        auto root_str = canonical_root.string();
        auto canon_str = canonical.string();

        if (canon_str.size() < root_str.size() ||
            canon_str.substr(0, root_str.size()) != root_str) {
            return path_;
        }

        return canonical.string();
    }

    OpenFile *SmbShare::find_open_file(const FileId & file_id)
    {
        std::lock_guard<std::mutex> lock(files_mutex_);
        auto it = open_files_.find(file_id.persistent);
        if (it != open_files_.end()) {
            return &it->second;
        }
        return nullptr;
    }

    void SmbShare::add_open_file(const FileId & file_id, OpenFile file)
    {
        std::lock_guard<std::mutex> lock(files_mutex_);
        open_files_[file_id.persistent] = std::move(file);
    }

    void SmbShare::remove_open_file(const FileId & file_id)
    {
        std::lock_guard<std::mutex> lock(files_mutex_);
        open_files_.erase(file_id.persistent);
    }

    std::vector<FileId> SmbShare::all_open_file_ids() const
    {
        std::lock_guard<std::mutex> lock(files_mutex_);
        std::vector<FileId> ids;
        ids.reserve(open_files_.size());
        for (const auto & [
                              key,
                              file
                          ] : open_files_) {
            ids.push_back(file.file_id);
        }
        return ids;
    }

    void SmbShareManager::add_share(const SmbShareConfig & config)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto share = std::make_shared<SmbShare>(config);
        shares_[config.name] = std::move(share);
    }

    void SmbShareManager::remove_share(const std::string & name)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        shares_.erase(name);
    }

    SmbShare *SmbShareManager::find_share(const std::string & name)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = shares_.find(name);
        if (it != shares_.end()) {
            return yuan::base::owner_ptr(it->second);
        }
        return nullptr;
    }

    std::shared_ptr<SmbShare> SmbShareManager::find_share_owner(const std::string & name)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = shares_.find(name);
        if (it != shares_.end()) {
            return it->second;
        }
        return {};
    }

    std::vector<SmbShare *> SmbShareManager::list_shares()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<SmbShare *> result;
        result.reserve(shares_.size());
        for (const auto & [
                              name,
                              share
                          ] : shares_) {
            result.push_back(yuan::base::owner_ptr(share));
        }
        return result;
    }

    SmbShare *SmbShareManager::find_share_by_type(ShareType type)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto & [
                              name,
                              share
                          ] : shares_) {
            if (share->type() == type) {
                return yuan::base::owner_ptr(share);
            }
        }
        return nullptr;
    }
}
