#ifndef __NET_SHADOWSOCKS_SHADOWSOCKS_SESSION_H__
#define __NET_SHADOWSOCKS_SHADOWSOCKS_SESSION_H__

#include "shadowsocks_protocol.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace yuan::net
{
    class Connection;
}

namespace yuan::net::shadowsocks
{
    class ShadowsocksSession
    {
    public:
        enum class State {
            reading_salt,
            reading_request,
            connecting,
            established,
            closed
        };

    public:
        explicit ShadowsocksSession(std::shared_ptr<Connection> client_conn);
        ~ShadowsocksSession();

        ShadowsocksSession(const ShadowsocksSession &) = delete;
        ShadowsocksSession &operator=(const ShadowsocksSession &) = delete;

        const std::shared_ptr<Connection> &client_connection() const;
        const std::shared_ptr<Connection> &remote_connection() const;
        void set_remote_connection(std::shared_ptr<Connection> conn);

        State state() const;
        void set_state(State s);

        const TargetAddress &target() const;
        void set_target(TargetAddress target);

        const std::vector<uint8_t> &send_nonce() const;
        std::vector<uint8_t> &mutable_send_nonce();

        const std::vector<uint8_t> &recv_nonce() const;
        std::vector<uint8_t> &mutable_recv_nonce();

        const std::vector<uint8_t> &recv_subkey() const;
        std::vector<uint8_t> &mutable_recv_subkey();

        const std::vector<uint8_t> &send_subkey() const;
        std::vector<uint8_t> &mutable_send_subkey();

        bool request_target_parsed() const;
        void set_request_target_parsed(bool parsed);

    private:
        std::shared_ptr<Connection> client_conn_;
        std::shared_ptr<Connection> remote_conn_;
        State state_ = State::reading_salt;
        TargetAddress target_;
        std::vector<uint8_t> send_nonce_;
        std::vector<uint8_t> recv_nonce_;
        std::vector<uint8_t> recv_subkey_;
        std::vector<uint8_t> send_subkey_;
        bool request_target_parsed_ = false;
    };
}

#endif
