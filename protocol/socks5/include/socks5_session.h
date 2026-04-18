#ifndef __NET_SOCKS5_SOCKS5_SESSION_H__
#define __NET_SOCKS5_SOCKS5_SESSION_H__

#include "socks5_protocol.h"

#include <cstdint>
#include <memory>
#include <string>

namespace yuan::net
{
    class Connection;
}

namespace yuan::net::socks5
{
    class Socks5Session
    {
    public:
        enum class State {
            greeting,
            auth,
            request,
            connecting,
            udp_associate,
            established,
            closed
        };

    public:
        explicit Socks5Session(std::shared_ptr<Connection> client_conn);
        ~Socks5Session();

        Socks5Session(const Socks5Session &) = delete;
        Socks5Session &operator=(const Socks5Session &) = delete;

    public:
        const std::shared_ptr<Connection> &client_connection() const
        {
            return client_conn_;
        }
        const std::shared_ptr<Connection> &remote_connection() const
        {
            return remote_conn_;
        }
        void set_remote_connection(std::shared_ptr<Connection> conn)
        {
            remote_conn_ = std::move(conn);
        }

        State state() const
        {
            return state_;
        }
        void set_state(State s)
        {
            state_ = s;
        }

        const std::string &target_host() const
        {
            return target_host_;
        }
        void set_target_host(const std::string &host)
        {
            target_host_ = host;
        }

        uint16_t target_port() const
        {
            return target_port_;
        }
        void set_target_port(uint16_t port)
        {
            target_port_ = port;
        }

        Command command() const
        {
            return command_;
        }
        void set_command(Command cmd)
        {
            command_ = cmd;
        }

        AddressType address_type() const
        {
            return atyp_;
        }
        void set_address_type(AddressType atyp)
        {
            atyp_ = atyp;
        }

        const std::string &username() const
        {
            return username_;
        }
        void set_username(const std::string &name)
        {
            username_ = name;
        }

        const std::string &password() const
        {
            return password_;
        }
        void set_password(const std::string &pwd)
        {
            password_ = pwd;
        }

    private:
        std::shared_ptr<Connection> client_conn_;
        std::shared_ptr<Connection> remote_conn_;
        State state_;
        std::string target_host_;
        uint16_t target_port_;
        Command command_;
        AddressType atyp_;
        std::string username_;
        std::string password_;
    };
}

#endif
