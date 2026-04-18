#include "dns_server.h"

int main()
{
    yuan::net::dns::DnsServer svr;
    svr.serve(9090);
    return 0;
}