#ifndef __NET_SMB_SMB_SHARE_H__
#define __NET_SMB_SMB_SHARE_H__

#include "smb_config.h"
#include "protocol/smb2_constants.h"
#include "protocol/smb2_structures.h"
#include "smb_file_system.h"
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace yuan::net::smb
{
    class SmbFileSystem;

    struct OpenFile
    {
        FileId file_id;
        std::string path;
        uint32_t access_mask = 0;
        uint32_t share_access = 0;
        uint32_t create_disposition = 0;
        uint32_t file_attributes = 0;
        bool is_directory = false;
        uint8_t oplock_level = SMB2_OPLOCK_LEVEL_NONE;
        uint8_t lease_key[SMB2_LEASE_KEY_SIZE] = {};
        uint32_t lease_state = 0;
        void *file_handle = nullptr;
        bool durable = false;
        uint32_t durable_timeout_ms = 0;
        uint32_t tree_id = 0;
    };

    class SmbShare
    {
    public:
        SmbShare(const SmbShareConfig &config);
        ~SmbShare();

        const std::string &name() const
        {
            return name_;
        }
        const std::string &comment() const
        {
            return comment_;
        }
        ShareType type() const
        {
            return type_;
        }
        const std::string &path() const
        {
            return path_;
        }
        uint32_t share_flags() const
        {
            return share_flags_;
        }
        uint32_t capabilities() const
        {
            return capabilities_;
        }
        SmbFileSystem *file_system() const
        {
            return file_system_;
        }
        void set_file_system(SmbFileSystem *fs)
        {
            file_system_ = fs;
        }

        std::string resolve_path(const std::u16string &relative) const;
        std::string resolve_path(const std::string &relative) const;

        OpenFile *find_open_file(const FileId &file_id);
        void add_open_file(const FileId &file_id, OpenFile file);
        void remove_open_file(const FileId &file_id);
        std::vector<FileId> all_open_file_ids() const;

        int current_uses() const
        {
            return current_uses_;
        }
        int max_uses() const
        {
            return max_uses_;
        }
        void increment_uses()
        {
            ++current_uses_;
        }
        void decrement_uses()
        {
            if (current_uses_ > 0)
                --current_uses_;
        }

    private:
        std::string name_;
        std::string comment_;
        ShareType type_;
        std::string path_;
        uint32_t share_flags_;
        uint32_t capabilities_;
        int max_uses_;
        int current_uses_ = 0;
        SmbFileSystem *file_system_ = nullptr;
        mutable std::mutex files_mutex_;
        std::unordered_map<uint64_t, OpenFile> open_files_;
    };

    class SmbShareManager
    {
    public:
        void add_share(const SmbShareConfig &config);
        void remove_share(const std::string &name);
        SmbShare *find_share(const std::string &name);
        std::vector<SmbShare *> list_shares();
        SmbShare *find_share_by_type(ShareType type);

    private:
        mutable std::mutex mutex_;
        std::unordered_map<std::string, std::unique_ptr<SmbShare> > shares_;
    };
}
#endif
