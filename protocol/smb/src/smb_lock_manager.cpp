#include "smb_lock_manager.h"
#include <algorithm>
#include <cstring>
#include <functional>

namespace yuan::net::smb
{
    static uint64_t lease_key_hash(const uint8_t key[SMB2_LEASE_KEY_SIZE])
    {
        uint64_t h = 0xcbf29ce484222325ULL;
        for (int i = 0; i < SMB2_LEASE_KEY_SIZE; ++i) {
            h ^= key[i];
            h *= 0x100000001b3ULL;
        }
        return h;
    }

    bool SmbLockManager::locks_overlap(uint64_t off1, uint64_t len1, uint64_t off2, uint64_t len2) const
    {
        if (len1 == 0 || len2 == 0)
            return false;
        const uint64_t end1 = off1 > UINT64_MAX - len1 ? UINT64_MAX : off1 + len1;
        const uint64_t end2 = off2 > UINT64_MAX - len2 ? UINT64_MAX : off2 + len2;
        if (off1 >= end2)
            return false;
        if (off2 >= end1)
            return false;
        return true;
    }

    bool SmbLockManager::lock_conflicts(const ByteRangeLock & a, const ByteRangeLock & b) const
    {
        if (!locks_overlap(a.offset, a.length, b.offset, b.length))
            return false;
        if (a.exclusive || b.exclusive)
            return true;
        return false;
    }

    NtStatus SmbLockManager::request_lock(const FileId & file_id, uint64_t session_id, uint32_t tree_id,
                                          uint64_t offset, uint64_t length, bool exclusive)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto key = file_id.persistent;
        ByteRangeLock new_lock{ offset, length, exclusive, session_id, tree_id };

        auto it = locks_.find(key);
        if (it != locks_.end()) {
            for (const auto &existing : it->second) {
                if (lock_conflicts(new_lock, existing)) {
                    return NtStatus::LOCK_NOT_GRANTED;
                }
            }
        }

        locks_[key].push_back(new_lock);
        return NtStatus::SUCCESS;
    }

    NtStatus SmbLockManager::release_lock(const FileId & file_id, uint64_t session_id,
                                          uint64_t offset, uint64_t length)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto key = file_id.persistent;
        auto it = locks_.find(key);
        if (it == locks_.end())
            return NtStatus::RANGE_NOT_LOCKED;

        auto &vec = it->second;
        auto lit = std::find_if(vec.begin(), vec.end(), [&](const ByteRangeLock &l) {
            return l.session_id == session_id && l.offset == offset && l.length == length;
        });

        if (lit == vec.end())
            return NtStatus::RANGE_NOT_LOCKED;
        vec.erase(lit);
        return NtStatus::SUCCESS;
    }

    NtStatus SmbLockManager::release_all_locks(const FileId & file_id, uint64_t session_id)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto key = file_id.persistent;
        auto it = locks_.find(key);
        if (it == locks_.end())
            return NtStatus::SUCCESS;

        auto &vec = it->second;
        vec.erase(std::remove_if(vec.begin(), vec.end(),
                                 [&](const ByteRangeLock &l) { return l.session_id == session_id; }),
                  vec.end());
        return NtStatus::SUCCESS;
    }

    NtStatus SmbLockManager::request_oplock(const FileId & file_id, uint64_t session_id, uint8_t level)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto key = file_id.persistent;
        auto it = oplocks_.find(key);
        if (it != oplocks_.end()) {
            it->second.break_pending = true;
            return NtStatus::OBJECT_NAME_COLLISION;
        }

        OplockEntry entry;
        entry.file_id = file_id;
        entry.current_level = level;
        entry.requested_level = level;
        entry.session_id = session_id;
        entry.break_pending = false;
        oplocks_[key] = entry;
        return NtStatus::SUCCESS;
    }

    NtStatus SmbLockManager::ack_oplock_break(const FileId & file_id, uint64_t session_id, uint8_t level)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto key = file_id.persistent;
        auto it = oplocks_.find(key);
        if (it == oplocks_.end())
            return NtStatus::NOT_FOUND;

        it->second.current_level = level;
        it->second.break_pending = false;
        return NtStatus::SUCCESS;
    }

    std::optional<OplockEntry> SmbLockManager::get_oplock(const FileId & file_id) const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = oplocks_.find(file_id.persistent);
        if (it != oplocks_.end()) {
            return it->second;
        }
        return std::nullopt;
    }

    void SmbLockManager::remove_oplock(const FileId & file_id)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        oplocks_.erase(file_id.persistent);
    }

    NtStatus SmbLockManager::request_lease(const FileId & file_id, uint64_t session_id,
                                           const uint8_t lease_key[SMB2_LEASE_KEY_SIZE], uint32_t state)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto kh = lease_key_hash(lease_key);
        auto it = leases_.find(kh);
        if (it != leases_.end()) {
            if (it->second.session_id != session_id) {
                it->second.break_pending = true;
            }
            return NtStatus::OBJECT_NAME_COLLISION;
        }

        LeaseEntry entry;
        std::memcpy(entry.lease_key, lease_key, SMB2_LEASE_KEY_SIZE);
        entry.current_state = state;
        entry.requested_state = state;
        entry.file_id = file_id;
        entry.session_id = session_id;
        entry.break_pending = false;
        leases_[kh] = entry;
        return NtStatus::SUCCESS;
    }

    NtStatus SmbLockManager::ack_lease_break(const uint8_t lease_key[SMB2_LEASE_KEY_SIZE], uint64_t session_id, uint32_t state)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto kh = lease_key_hash(lease_key);
        auto it = leases_.find(kh);
        if (it == leases_.end())
            return NtStatus::NOT_FOUND;

        it->second.current_state = state;
        it->second.break_pending = false;
        return NtStatus::SUCCESS;
    }

    std::optional<LeaseEntry> SmbLockManager::get_lease(const uint8_t lease_key[SMB2_LEASE_KEY_SIZE]) const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto kh = lease_key_hash(lease_key);
        auto it = leases_.find(kh);
        if (it != leases_.end()) {
            return it->second;
        }
        return std::nullopt;
    }

    void SmbLockManager::remove_lease(const FileId & file_id)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto it = leases_.begin(); it != leases_.end(); ++it) {
            if (it->second.file_id.persistent == file_id.persistent) {
                leases_.erase(it);
                return;
            }
        }
    }

    bool SmbLockManager::is_range_locked(const FileId & file_id, uint64_t offset, uint64_t length, bool write_access) const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = locks_.find(file_id.persistent);
        if (it == locks_.end())
            return false;

        for (const auto &l : it->second) {
            if (locks_overlap(offset, length, l.offset, l.length)) {
                if (l.exclusive || write_access) {
                    return true;
                }
            }
        }
        return false;
    }
}
