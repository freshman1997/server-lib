#include "http_client.h"

#include <cstdlib>
#include <iostream>
#include <string>

namespace
{
    bool check(bool condition, const char *message)
    {
        if (!condition) {
            std::cerr << message << '\n';
            return false;
        }
        return true;
    }
}

int main()
{
    yuan::net::http::HttpClient client;
    bool ok = true;

    ok &= check(client.query("http://example.com"), "plain host should parse");
    ok &= check(client.query("http://example.com:8080/path?q=1"), "explicit http port should parse");
    ok &= check(client.query("https://example.com:8443/api"), "https custom port should parse");
    ok &= check(client.query("http://[::1]:8080/"), "bracketed IPv6 host should parse");

    ok &= check(!client.query("ftp://example.com"), "unsupported scheme should fail");
    ok &= check(!client.query("http://"), "empty authority should fail");
    ok &= check(!client.query("http://:80/"), "empty host should fail");
    ok &= check(!client.query("http://example.com:/"), "empty port should fail");
    ok &= check(!client.query("http://example.com:abc/"), "non-numeric port should fail");
    ok &= check(!client.query("http://example.com:70000/"), "out-of-range port should fail");
    ok &= check(!client.query("http://::1:8080/"), "unbracketed IPv6 should fail explicitly");

    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
