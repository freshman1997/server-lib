#include "shadowsocks_session.h"

#include "net/connection/connection.h"

namespace yuan::net::shadowsocks
{
    ShadowsocksSession::ShadowsocksSession(std::shared_ptr<Connection> client_conn)
        : client_conn_(std::move(client_conn))
    {
    }

    ShadowsocksSession::~ShadowsocksSession() = default;

    const std::shared_ptr<Connection> &ShadowsocksSession::client_connection() const
    {
        return client_conn_;
    }

    const std::shared_ptr<Connection> &ShadowsocksSession::remote_connection() const
    {
        return remote_conn_;
    }

    void ShadowsocksSession::set_remote_connection(std::shared_ptr<Connection> conn)
    {
        remote_conn_ = std::move(conn);
    }

    ShadowsocksSession::State ShadowsocksSession::state() const
    {
        return state_;
    }

    void ShadowsocksSession::set_state(State s)
    {
        state_ = s;
    }

    const TargetAddress &ShadowsocksSession::target() const
    {
        return target_;
    }

    void ShadowsocksSession::set_target(TargetAddress target)
    {
        target_ = std::move(target);
    }

    const std::vector<uint8_t> &ShadowsocksSession::send_nonce() const
    {
        return send_nonce_;
    }

    std::vector<uint8_t> &ShadowsocksSession::mutable_send_nonce()
    {
        return send_nonce_;
    }

    const std::vector<uint8_t> &ShadowsocksSession::recv_nonce() const
    {
        return recv_nonce_;
    }

    std::vector<uint8_t> &ShadowsocksSession::mutable_recv_nonce()
    {
        return recv_nonce_;
    }

    const std::vector<uint8_t> &ShadowsocksSession::recv_subkey() const
    {
        return recv_subkey_;
    }

    std::vector<uint8_t> &ShadowsocksSession::mutable_recv_subkey()
    {
        return recv_subkey_;
    }

    const std::vector<uint8_t> &ShadowsocksSession::send_subkey() const
    {
        return send_subkey_;
    }

    std::vector<uint8_t> &ShadowsocksSession::mutable_send_subkey()
    {
        return send_subkey_;
    }

    bool ShadowsocksSession::request_target_parsed() const
    {
        return request_target_parsed_;
    }

    void ShadowsocksSession::set_request_target_parsed(bool parsed)
    {
        request_target_parsed_ = parsed;
    }
}
