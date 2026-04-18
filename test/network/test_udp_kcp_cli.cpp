#include "dns_client.h"

int main()
{
    yuan::net::dns::DnsClient cli;
    cli.connect("127.0.0.1", 9090);
    return 0;
}