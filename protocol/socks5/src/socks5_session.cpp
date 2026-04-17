#include "socks5_session.h"
#include "net/connection/connection.h"

namespace yuan::net::socks5
{
    Socks5Session::Socks5Session(Connection * client_conn)
        : client_conn_(client_conn), remote_conn_(nullptr), state_(State::greeting), target_port_(0), command_(Command::connect), atyp_(AddressType::ipv4)
    {
    }

    Socks5Session::~Socks5Session() = default;
}
