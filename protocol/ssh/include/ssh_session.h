#ifndef __NET_SSH_SSH_SESSION_H__
#define __NET_SSH_SSH_SESSION_H__

#include "auth/ssh_authenticator.h"
#include "connection/ssh_connection_manager.h"
#include "protocol/ssh_constants.h"
#include "protocol/ssh_structures.h"
#include "transport/ssh_transport.h"
#include "buffer/byte_buffer.h"
#include "coroutine/runtime_view.h"
#include "net/connection/connection.h"
#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace yuan::net::ssh
{
    using ::yuan::buffer::ByteBuffer;

    class SshHandler;
    class SshServer;

    class SshSession
    {
    public:
        enum class State {
            connected,
            version_exchanged,
            kex_init,
            kex_in_progress,
            newkeys,
            auth_start,
            authenticating,
            auth_need_more,
            auth_success,
            active,
            disconnected
        };

        SshSession(uint64_t session_id, SshServer *server);
        ~SshSession();

        SshSession(const SshSession &) = delete;
        SshSession &operator=(const SshSession &) = delete;

        uint64_t session_id() const
        {
            return session_id_;
        }
        State state() const
        {
            return state_;
        }
        void set_state(State s)
        {
            state_ = s;
        }

        State pre_rekey_state() const
        {
            return pre_rekey_state_;
        }
        void set_pre_rekey_state(State s)
        {
            pre_rekey_state_ = s;
        }

        const std::string &username() const
        {
            return authenticator_.username();
        }
        const std::vector<uint8_t> &session_id_proto() const
        {
            return transport_.session_id();
        }
        bool authenticated() const
        {
            return authenticator_.authenticated();
        }

        SshTransport &transport()
        {
            return transport_;
        }
        SshAuthenticator &authenticator()
        {
            return authenticator_;
        }
        SshConnectionManager &connection_manager()
        {
            return conn_mgr_;
        }

        SshServer *server()
        {
            return server_;
        }

        void set_client_connection(const std::shared_ptr<net::Connection> &conn)
        {
            client_conn_owner_ = conn;
            client_conn_ = conn ? &*conn : nullptr;
        }
        void set_client_connection(net::Connection *conn)
        {
            client_conn_owner_.reset();
            client_conn_ = conn;
        }

        std::shared_ptr<net::Connection> client_connection() const
        {
            return client_conn_owner_.lock();
        }

        void set_runtime(coroutine::RuntimeView rv)
        {
            runtime_ = rv;
        }

        coroutine::RuntimeView runtime() const
        {
            return runtime_;
        }

        void dispatch(SshMessageType msg_type, const std::vector<uint8_t> &payload, SshHandler *handler);

        void enqueue_outgoing(ByteBuffer buf);
        std::vector<ByteBuffer> drain_outgoing();
        void flush_channel_pending_data();

        ByteBuffer build_service_accept(const std::string &service_name) const;
        ByteBuffer build_userauth_success() const;
        ByteBuffer build_userauth_failure(bool partial_success) const;
        ByteBuffer build_userauth_pk_ok(const std::string &algo, const std::vector<uint8_t> &key_blob) const;
        ByteBuffer build_userauth_info_request(const SshUserauthInfoRequestMessage &msg) const;
        ByteBuffer build_disconnect(SshDisconnectReason reason, const std::string &description) const;

    private:
        void handle_service_request(const std::vector<uint8_t> &payload, SshHandler *handler);
        void handle_userauth_request(const std::vector<uint8_t> &payload, SshHandler *handler);
        void handle_userauth_info_response(const std::vector<uint8_t> &payload, SshHandler *handler);
        void handle_connection_message(SshMessageType msg_type, const std::vector<uint8_t> &payload, SshHandler *handler);

        uint64_t session_id_;
        SshServer *server_;
        State state_ = State::connected;
        State pre_rekey_state_ = State::connected;

        SshTransport transport_;
        SshAuthenticator authenticator_;
        SshConnectionManager conn_mgr_;

        std::weak_ptr<net::Connection> client_conn_owner_;
        net::Connection *client_conn_ = nullptr;
        coroutine::RuntimeView runtime_;

        std::mutex outgoing_mutex_;
        std::deque<ByteBuffer> outgoing_;
    };

    class SshSessionManager
    {
    public:
        SshSessionManager() = default;
        ~SshSessionManager() = default;

        SshSession *create_session(SshServer *server);
        SshSession *find_session(uint64_t session_id);
        void remove_session(uint64_t session_id);
        void close_all();

        uint32_t session_count() const;
        bool session_limit_reached(uint32_t max_sessions) const;

    private:
        mutable std::mutex mutex_;
        std::unordered_map<uint64_t, std::unique_ptr<SshSession> > sessions_;
        std::atomic<uint64_t> next_session_id_{ 1 };
    };
}

#endif
