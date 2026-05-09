#ifndef __NET_SMB_SMB_SESSION_H__
#define __NET_SMB_SMB_SESSION_H__

#include "protocol/smb2_constants.h"
#include "protocol/smb2_structures.h"
#include "auth/smb_auth.h"
#include "crypto/smb_crypto.h"
#include "crypto/smb_key_derivation.h"
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace yuan::net
{
    class Connection;
}

namespace yuan::net::smb
{
    class SmbShare;
    class SmbLockManager;

    struct TreeConnection
    {
        uint32_t tree_id = 0;
        std::string share_name;
        std::shared_ptr<SmbShare> share_owner;
        SmbShare *share = nullptr;
        bool is_dfs = false;
        bool is_ca = false;
    };

    class SmbSession
    {
    public:
        enum class State {
            connected,
            negotiating,
            authenticating,
            authenticated,
            active,
            closed
        };

        SmbSession(uint64_t session_id, Connection *conn);
        SmbSession(uint64_t session_id, const std::shared_ptr<Connection> &conn);
        ~SmbSession();

        uint64_t session_id() const
        {
            return session_id_;
        }
        std::shared_ptr<Connection> connection() const
        {
            return conn_owner_.lock();
        }
        std::shared_ptr<Connection> connection_owner() const
        {
            return conn_owner_.lock();
        }
        State state() const
        {
            return state_;
        }
        void set_state(State s)
        {
            state_ = s;
        }

        DialectRevision dialect() const
        {
            return dialect_;
        }
        void set_dialect(DialectRevision d)
        {
            dialect_ = d;
        }

        uint16_t credits_available() const
        {
            return credits_available_;
        }
        void grant_credits(uint16_t n)
        {
            credits_available_ += n;
        }
        uint16_t consume_credits(uint16_t n);

        void set_auth(std::unique_ptr<SmbAuth> auth)
        {
            auth_ = std::move(auth);
        }
        SmbAuth *auth()
        {
            return auth_ ? &*auth_ : nullptr;
        }

        void set_signing_key(std::vector<uint8_t> key)
        {
            signing_key_ = std::move(key);
        }
        const std::vector<uint8_t> &signing_key() const
        {
            return signing_key_;
        }
        void set_encryption_key(std::vector<uint8_t> key)
        {
            encryption_key_ = std::move(key);
        }
        const std::vector<uint8_t> &encryption_key() const
        {
            return encryption_key_;
        }
        void set_decryption_key(std::vector<uint8_t> key)
        {
            decryption_key_ = std::move(key);
        }
        const std::vector<uint8_t> &decryption_key() const
        {
            return decryption_key_;
        }
        void set_encryption_iv(std::vector<uint8_t> iv)
        {
            encryption_iv_ = std::move(iv);
        }
        const std::vector<uint8_t> &encryption_iv() const
        {
            return encryption_iv_;
        }
        void set_decryption_iv(std::vector<uint8_t> iv)
        {
            decryption_iv_ = std::move(iv);
        }
        const std::vector<uint8_t> &decryption_iv() const
        {
            return decryption_iv_;
        }

        void set_preauth_hash(std::vector<uint8_t> hash)
        {
            preauth_hash_ = std::move(hash);
        }
        const std::vector<uint8_t> &preauth_hash() const
        {
            return preauth_hash_;
        }

        bool is_encrypted() const
        {
            return !encryption_key_.empty();
        }
        bool is_signed() const
        {
            return !signing_key_.empty();
        }

        uint32_t add_tree_connection(TreeConnection tree);
        TreeConnection *find_tree(uint32_t tree_id);
        void remove_tree(uint32_t tree_id);
        std::vector<uint32_t> all_tree_ids() const;

        FileId allocate_file_id();
        void set_user_name(const std::string &name)
        {
            user_name_ = name;
        }
        const std::string &user_name() const
        {
            return user_name_;
        }
        void set_domain_name(const std::string &name)
        {
            domain_name_ = name;
        }
        const std::string &domain_name() const
        {
            return domain_name_;
        }

        void set_server_capabilities(uint32_t caps)
        {
            server_capabilities_ = caps;
        }
        uint32_t server_capabilities() const
        {
            return server_capabilities_;
        }
        void set_server_security_mode(uint16_t mode)
        {
            server_security_mode_ = mode;
        }
        uint16_t server_security_mode() const
        {
            return server_security_mode_;
        }

        void set_signing_algorithm(uint16_t algo)
        {
            signing_algorithm_ = algo;
        }
        uint16_t signing_algorithm() const
        {
            return signing_algorithm_;
        }

        void set_crypto(std::shared_ptr<SmbCrypto> crypto)
        {
            crypto_ = crypto;
        }
        SmbCrypto *crypto()
        {
            return crypto_ ? &*crypto_ : nullptr;
        }

        void close();

    private:
        uint64_t session_id_;
        std::weak_ptr<Connection> conn_owner_;
        Connection *conn_;
        State state_ = State::connected;
        DialectRevision dialect_ = DialectRevision::SMB_2_002;
        std::atomic<uint16_t> credits_available_{ 0 };
        std::unique_ptr<SmbAuth> auth_;
        std::vector<uint8_t> signing_key_;
        std::vector<uint8_t> encryption_key_;
        std::vector<uint8_t> decryption_key_;
        std::vector<uint8_t> encryption_iv_;
        std::vector<uint8_t> decryption_iv_;
        std::vector<uint8_t> preauth_hash_;
        std::string user_name_;
        std::string domain_name_;
        uint32_t server_capabilities_ = 0;
        uint16_t server_security_mode_ = 0;
        uint16_t signing_algorithm_ = 0;
        std::shared_ptr<SmbCrypto> crypto_;
        mutable std::mutex trees_mutex_;
        std::unordered_map<uint32_t, TreeConnection> trees_;
        std::atomic<uint64_t> next_file_id_{ 1 };
    };

    class SmbSessionManager
    {
    public:
        SmbSessionManager() = default;
        ~SmbSessionManager() = default;

        SmbSession *create_session(Connection *conn);
        SmbSession *create_session(const std::shared_ptr<Connection> &conn);
        SmbSession *find_session(uint64_t session_id);
        void remove_session(uint64_t session_id);
        void close_all();

    private:
        mutable std::mutex mutex_;
        std::unordered_map<uint64_t, std::unique_ptr<SmbSession> > sessions_;
        std::atomic<uint64_t> next_session_id_{ 1 };
    };
}
#endif
