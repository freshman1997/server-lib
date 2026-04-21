#include "smb_session.h"

namespace yuan::net::smb
{
    namespace
    {
        template <typename T>
        T *ptr_of(const std::shared_ptr<T> &owner)
        {
            return owner ? const_cast<T *>(&*owner) : nullptr;
        }

        template <typename T>
        T *ptr_of(const std::unique_ptr<T> &owner)
        {
            return owner ? const_cast<T *>(&*owner) : nullptr;
        }
    }

    SmbSession::SmbSession(uint64_t session_id, Connection * conn)
        : session_id_(session_id), conn_(conn)
    {
    }

    SmbSession::SmbSession(uint64_t session_id, const std::shared_ptr<Connection> &conn)
        : session_id_(session_id), conn_owner_(conn), conn_(ptr_of(conn))
    {
    }

    SmbSession::~SmbSession() = default;

    uint16_t SmbSession::consume_credits(uint16_t n)
    {
        uint16_t available = credits_available_.load();
        uint16_t to_consume = std::min(n, available);
        credits_available_ -= to_consume;
        return to_consume;
    }

    uint32_t SmbSession::add_tree_connection(TreeConnection tree)
    {
        std::lock_guard<std::mutex> lock(trees_mutex_);
        static uint32_t next_tree_id = 1;
        tree.tree_id = next_tree_id++;
        uint32_t tid = tree.tree_id;
        trees_[tid] = std::move(tree);
        return tid;
    }

    TreeConnection *SmbSession::find_tree(uint32_t tree_id)
    {
        std::lock_guard<std::mutex> lock(trees_mutex_);
        auto it = trees_.find(tree_id);
        if (it != trees_.end()) {
            return &it->second;
        }
        return nullptr;
    }

    void SmbSession::remove_tree(uint32_t tree_id)
    {
        std::lock_guard<std::mutex> lock(trees_mutex_);
        trees_.erase(tree_id);
    }

    std::vector<uint32_t> SmbSession::all_tree_ids() const
    {
        std::lock_guard<std::mutex> lock(trees_mutex_);
        std::vector<uint32_t> ids;
        ids.reserve(trees_.size());
        for (const auto & [
                              id,
                              tree
                          ] : trees_) {
            ids.push_back(id);
        }
        return ids;
    }

    FileId SmbSession::allocate_file_id()
    {
        FileId fid;
        fid.persistent = next_file_id_.fetch_add(1);
        fid.volatile_id = fid.persistent;
        return fid;
    }

    void SmbSession::close()
    {
        state_ = State::closed;
        {
            std::lock_guard<std::mutex> lock(trees_mutex_);
            trees_.clear();
        }
        signing_key_.clear();
        encryption_key_.clear();
        decryption_key_.clear();
        encryption_iv_.clear();
        decryption_iv_.clear();
        preauth_hash_.clear();
    }

    SmbSession *SmbSessionManager::create_session(Connection * conn)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        uint64_t sid = next_session_id_.fetch_add(1);
        auto session = std::make_unique<SmbSession>(sid, conn);
        auto *ptr = ptr_of(session);
        sessions_[sid] = std::move(session);
        return ptr;
    }

    SmbSession *SmbSessionManager::create_session(const std::shared_ptr<Connection> &conn)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        uint64_t sid = next_session_id_.fetch_add(1);
        auto session = std::make_unique<SmbSession>(sid, conn);
        auto *ptr = ptr_of(session);
        sessions_[sid] = std::move(session);
        return ptr;
    }

    SmbSession *SmbSessionManager::find_session(uint64_t session_id)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = sessions_.find(session_id);
        if (it != sessions_.end()) {
            return ptr_of(it->second);
        }
        return nullptr;
    }

    void SmbSessionManager::remove_session(uint64_t session_id)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        sessions_.erase(session_id);
    }

    void SmbSessionManager::close_all()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto & [
                        id,
                        session
                    ] : sessions_) {
            session->close();
        }
        sessions_.clear();
    }
}
