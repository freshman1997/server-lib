#ifndef __NET_SMB_SMB_LOCK_MANAGER_H__
#define __NET_SMB_SMB_LOCK_MANAGER_H__

#include "protocol/smb2_constants.h"
#include "protocol/smb2_structures.h"
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>

namespace yuan::net::smb
{
    struct ByteRangeLock
    {
        uint64_t offset = 0;
        uint64_t length = 0;
        bool exclusive = false;
        uint64_t session_id = 0;
        uint32_t tree_id = 0;
    };

    struct OplockEntry
    {
        FileId file_id;
        uint8_t current_level = SMB2_OPLOCK_LEVEL_NONE;
        uint8_t requested_level = SMB2_OPLOCK_LEVEL_NONE;
        uint64_t session_id = 0;
        bool break_pending = false;
    };

    struct LeaseEntry
    {
        uint8_t lease_key[SMB2_LEASE_KEY_SIZE] = {};
        uint32_t current_state = 0;
        uint32_t requested_state = 0;
        FileId file_id;
        uint64_t session_id = 0;
        bool break_pending = false;
    };

    class SmbLockManager
    {
    public:
        NtStatus request_lock(const FileId &file_id, uint64_t session_id, uint32_t tree_id,
                              uint64_t offset, uint64_t length, bool exclusive);
        NtStatus release_lock(const FileId &file_id, uint64_t session_id,
                              uint64_t offset, uint64_t length);
        NtStatus release_all_locks(const FileId &file_id, uint64_t session_id);

        NtStatus request_oplock(const FileId &file_id, uint64_t session_id, uint8_t level);
        NtStatus ack_oplock_break(const FileId &file_id, uint64_t session_id, uint8_t level);
        std::optional<OplockEntry> get_oplock(const FileId &file_id) const;
        void remove_oplock(const FileId &file_id);

        NtStatus request_lease(const FileId &file_id, uint64_t session_id,
                               const uint8_t lease_key[SMB2_LEASE_KEY_SIZE], uint32_t state);
        NtStatus ack_lease_break(const uint8_t lease_key[SMB2_LEASE_KEY_SIZE], uint64_t session_id, uint32_t state);
        std::optional<LeaseEntry> get_lease(const uint8_t lease_key[SMB2_LEASE_KEY_SIZE]) const;
        void remove_lease(const FileId &file_id);

        bool is_range_locked(const FileId &file_id, uint64_t offset, uint64_t length, bool write_access) const;

    private:
        bool locks_overlap(uint64_t off1, uint64_t len1, uint64_t off2, uint64_t len2) const;
        bool lock_conflicts(const ByteRangeLock &a, const ByteRangeLock &b) const;

        mutable std::mutex mutex_;
        std::unordered_map<uint64_t, std::vector<ByteRangeLock> > locks_;
        std::unordered_map<uint64_t, OplockEntry> oplocks_;
        std::unordered_map<uint64_t, LeaseEntry> leases_;
    };
}
#endif
