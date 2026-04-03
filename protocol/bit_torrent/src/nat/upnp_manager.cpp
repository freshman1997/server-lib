#include "nat/upnp_manager.h"
#include <cstring>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "iphlpapi.lib")
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <net/route.h>
#include <unistd.h>
#include <ifaddrs.h>
#endif

namespace yuan::net::bit_torrent
{

UpnpManager::UpnpManager()
    : running_(false),
      mapped_(false),
      internal_port_(0),
      mapped_port_(0),
      nat_pmp_available_(false),
      nat_pmp_lifetime_(0)
{
}

UpnpManager::~UpnpManager()
{
    stop();
}

void UpnpManager::start(const NatConfig &config, uint16_t internal_port, ResultCallback cb)
{
    config_ = config;
    internal_port_ = internal_port;
    result_cb_ = std::move(cb);
    mapped_ = false;

    if (!config_.enable_upnp && !config_.enable_nat_pmp)
    {
        if (result_cb_) result_cb_(false, "", 0);
        return;
    }

    running_ = true;

    // Run discovery in a background thread (SSDP can block for timeout_ms)
    worker_ = std::thread([this]()
    {
        if (config_.enable_upnp)
        {
            discover_igd();
        }

        if (!mapped_.load() && config_.enable_nat_pmp)
        {
            try_nat_pmp();
        }

        if (result_cb_)
        {
            std::lock_guard<std::mutex> lock(mutex_);
            result_cb_(mapped_.load(), external_ip_, mapped_port_);
        }
    });

    // Start renewal thread
    if (running_.load())
    {
        // Detach the worker thread - it's one-shot
        worker_.detach();
    }
}

void UpnpManager::stop()
{
    running_ = false;
    mapped_ = false;

    // Try to delete the port mapping
    if (!igd_control_url_.empty())
    {
        soap_delete_port_mapping();
    }

    if (!igd_control_url_.empty())
    {
        std::lock_guard<std::mutex> lock(mutex_);
        igd_control_url_.clear();
        igd_service_type_.clear();
        external_ip_.clear();
        mapped_port_ = 0;
    }
}

std::string UpnpManager::get_external_ip() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return external_ip_;
}

uint16_t UpnpManager::get_mapped_port() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return mapped_port_;
}

// ===== UPnP SSDP Discovery =====

bool UpnpManager::send_ssdp_discover(int sock)
{
    const char *ssdp_request =
        "M-SEARCH * HTTP/1.1\r\n"
        "HOST: 239.255.255.250:1900\r\n"
        "MAN: \"ssdp:discover\"\r\n"
        "MX: 3\r\n"
        "ST: upnp:rootdevice\r\n"
        "\r\n";

    struct sockaddr_in dest;
    std::memset(&dest, 0, sizeof(dest));
    dest.sin_family = AF_INET;
    dest.sin_port = htons(1900);
    dest.sin_addr.s_addr = inet_addr("239.255.255.250");

    // Allow multicast on the socket
    int ttl = 2;
    setsockopt(sock, IPPROTO_IP, IP_MULTICAST_TTL, (const char *)&ttl, sizeof(ttl));

    return sendto(sock, ssdp_request, (int)std::strlen(ssdp_request), 0,
                  (struct sockaddr *)&dest, sizeof(dest)) > 0;
}

void UpnpManager::discover_igd()
{
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif

    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) return;

    // Set receive timeout
#ifdef _WIN32
    DWORD timeout = config_.upnp_discover_timeout_ms;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout, sizeof(timeout));
#else
    struct timeval tv;
    tv.tv_sec = config_.upnp_discover_timeout_ms / 1000;
    tv.tv_usec = (config_.upnp_discover_timeout_ms % 1000) * 1000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif

    // Set reuse
    int reuse = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (const char *)&reuse, sizeof(reuse));

    if (!send_ssdp_discover(sock))
    {
        closesocket(sock);
#ifdef _WIN32
        WSACleanup();
#endif
        return;
    }

    // Receive SSDP responses
    char buf[2048];
    std::string location;

    while (running_.load())
    {
        struct sockaddr_in from;
        int fromlen = sizeof(from);
        int n = recvfrom(sock, buf, sizeof(buf) - 1, 0,
                         (struct sockaddr *)&from, &fromlen);

        if (n <= 0) break;

        buf[n] = '\0';
        std::string response(buf, n);

        // Check if this is a root device or WANIP/WANPPP service
        if (response.find("upnp:rootdevice") == std::string::npos &&
            response.find("urn:schemas-upnp-org:service:WANIPConnection") == std::string::npos &&
            response.find("urn:schemas-upnp-org:service:WANPPPConnection") == std::string::npos)
        {
            continue;
        }

        // Extract LOCATION header
        size_t loc_pos = response.find("LOCATION:");
        if (loc_pos == std::string::npos)
            loc_pos = response.find("location:");
        if (loc_pos != std::string::npos)
        {
            size_t start = loc_pos + 9;
            while (start < response.size() && (response[start] == ' ' || response[start] == '\t'))
                start++;
            size_t end = response.find("\r\n", start);
            if (end == std::string::npos) end = response.find('\n', start);
            if (end != std::string::npos)
            {
                location = response.substr(start, end - start);
                parse_ssdp_response(location);
                if (!igd_control_url_.empty()) break;
            }
        }
    }

    closesocket(sock);

    if (!igd_control_url_.empty())
    {
        // Try to get external IP
        soap_get_external_ip();

        // Add port mapping
        soap_add_port_mapping();
    }

#ifdef _WIN32
    WSACleanup();
#endif
}

void UpnpManager::parse_ssdp_response(const std::string &location)
{
    // Fetch the IGD description XML
    if (!fetch_igd_description(location))
        return;
}

bool UpnpManager::fetch_igd_description(const std::string &location)
{
    // Simple HTTP GET to fetch the IGD XML description
    // Parse out the host, port, and path
    std::string host, path;
    uint16_t port = 80;

    // Parse URL: http://host:port/path
    if (location.substr(0, 7) != "http://")
        return false;

    std::string rest = location.substr(7);
    size_t slash = rest.find('/');
    if (slash != std::string::npos)
    {
        path = rest.substr(slash);
        std::string host_port = rest.substr(0, slash);
        size_t colon = host_port.find(':');
        if (colon != std::string::npos)
        {
            host = host_port.substr(0, colon);
            port = static_cast<uint16_t>(std::atoi(host_port.substr(colon + 1).c_str()));
        }
        else
        {
            host = host_port;
        }
    }
    else
    {
        host = rest;
        path = "/";
    }

    // Create a TCP socket and fetch the XML
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return false;

    struct sockaddr_in server;
    std::memset(&server, 0, sizeof(server));
    server.sin_family = AF_INET;
    server.sin_port = htons(port);
    inet_pton(AF_INET, host.c_str(), &server.sin_addr);

#ifdef _WIN32
    DWORD timeout = 5000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout, sizeof(timeout));
#else
    struct timeval tv;
    tv.tv_sec = 5;
    tv.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif

    if (connect(sock, (struct sockaddr *)&server, sizeof(server)) < 0)
    {
        closesocket(sock);
        return false;
    }

    std::string request = "GET " + path + " HTTP/1.1\r\n"
                          "Host: " + host + "\r\n"
                          "Connection: close\r\n\r\n";
    send(sock, request.c_str(), (int)request.size(), 0);

    // Read response
    std::string xml;
    char buf[4096];
    int n;
    while ((n = recv(sock, buf, sizeof(buf) - 1, 0)) > 0)
    {
        buf[n] = '\0';
        xml += buf;
    }
    closesocket(sock);

    if (xml.empty()) return false;

    // Extract the body (after \r\n\r\n)
    size_t body_start = xml.find("\r\n\r\n");
    if (body_start != std::string::npos)
        xml = xml.substr(body_start + 4);

    return parse_igd_services(xml);
}

bool UpnpManager::parse_igd_services(const std::string &xml)
{
    // Look for WANIPConnection or WANPPPConnection service
    const char *wanip_service = "urn:schemas-upnp-org:service:WANIPConnection:1";
    const char *wanppp_service = "urn:schemas-upnp-org:service:WANPPPConnection:1";

    const char *target = nullptr;
    if (xml.find(wanip_service) != std::string::npos)
        target = wanip_service;
    else if (xml.find(wanppp_service) != std::string::npos)
        target = wanppp_service;
    else
        return false;

    // Extract the controlURL for the service
    // Find the <service> block containing our target serviceType
    size_t service_pos = xml.find(target);
    if (service_pos == std::string::npos) return false;

    // Go back to find the <service> tag
    size_t service_tag = xml.rfind("<service>", service_pos);
    if (service_tag == std::string::npos) return false;

    // Find the </service> closing tag
    size_t service_end = xml.find("</service>", service_tag);
    if (service_end == std::string::npos) return false;

    std::string service_block = xml.substr(service_tag, service_end - service_tag);

    // Extract controlURL
    size_t ctrl_pos = service_block.find("<controlURL>");
    if (ctrl_pos == std::string::npos) return false;

    size_t ctrl_start = ctrl_pos + 13;
    size_t ctrl_end = service_block.find("</controlURL>", ctrl_start);
    if (ctrl_end == std::string::npos) return false;

    std::string control_url = service_block.substr(ctrl_start, ctrl_end - ctrl_start);

    // Extract the base URL from the location for resolving relative URLs
    // For now, assume the controlURL is absolute (most IGDs provide this)
    igd_control_url_ = control_url;
    igd_service_type_ = target;

    return true;
}

std::string UpnpManager::http_request(const std::string &url, const std::string &soap_action,
                                       const std::string &soap_body)
{
    // Parse URL
    std::string host, path;
    uint16_t port = 80;

    if (url.substr(0, 7) == "http://")
    {
        std::string rest = url.substr(7);
        size_t slash = rest.find('/');
        if (slash != std::string::npos)
        {
            path = rest.substr(slash);
            std::string host_port = rest.substr(0, slash);
            size_t colon = host_port.find(':');
            if (colon != std::string::npos)
            {
                host = host_port.substr(0, colon);
                port = static_cast<uint16_t>(std::atoi(host_port.substr(colon + 1).c_str()));
            }
            else
            {
                host = host_port;
            }
        }
    }

    if (host.empty()) return "";

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return "";

    struct sockaddr_in server;
    std::memset(&server, 0, sizeof(server));
    server.sin_family = AF_INET;
    server.sin_port = htons(port);
    inet_pton(AF_INET, host.c_str(), &server.sin_addr);

#ifdef _WIN32
    DWORD timeout = 5000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout, sizeof(timeout));
#else
    struct timeval tv;
    tv.tv_sec = 5;
    tv.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif

    if (connect(sock, (struct sockaddr *)&server, sizeof(server)) < 0)
    {
        closesocket(sock);
        return "";
    }

    std::string request =
        "POST " + path + " HTTP/1.1\r\n"
        "Host: " + host + "\r\n"
        "Content-Type: text/xml; charset=\"utf-8\"\r\n"
        "SOAPAction: \"" + soap_action + "\"\r\n"
        "Content-Length: " + std::to_string(soap_body.size()) + "\r\n"
        "Connection: close\r\n\r\n" +
        soap_body;

    send(sock, request.c_str(), (int)request.size(), 0);

    std::string response;
    char buf[4096];
    int n;
    while ((n = recv(sock, buf, sizeof(buf) - 1, 0)) > 0)
    {
        buf[n] = '\0';
        response += buf;
    }
    closesocket(sock);

    return response;
}

bool UpnpManager::soap_add_port_mapping()
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (igd_control_url_.empty()) return false;

    std::string local_ip = get_local_ip();
    if (local_ip.empty()) return false;

    std::string ext_port = std::to_string(internal_port_);
    std::string int_port = std::to_string(internal_port_);
    std::string protocol = "TCP";
    std::string desc = "BitTorrent";

    std::string body =
        "<?xml version=\"1.0\"?>\r\n"
        "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
        "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">\r\n"
        "  <s:Body>\r\n"
        "    <u:AddPortMapping xmlns:u=\"" + igd_service_type_ + "\">\r\n"
        "      <NewRemoteHost></NewRemoteHost>\r\n"
        "      <NewExternalPort>" + ext_port + "</NewExternalPort>\r\n"
        "      <NewProtocol>" + protocol + "</NewProtocol>\r\n"
        "      <NewInternalPort>" + int_port + "</NewInternalPort>\r\n"
        "      <NewInternalClient>" + local_ip + "</NewInternalClient>\r\n"
        "      <NewEnabled>1</NewEnabled>\r\n"
        "      <NewPortMappingDescription>" + desc + "</NewPortMappingDescription>\r\n"
        "      <NewLeaseDuration>" + std::to_string(config_.upnp_lease_duration) + "</NewLeaseDuration>\r\n"
        "    </u:AddPortMapping>\r\n"
        "  </s:Body>\r\n"
        "</s:Envelope>\r\n";

    std::string action = igd_service_type_ + "#AddPortMapping";
    std::string response = http_request(igd_control_url_, action, body);

    if (!response.empty() && response.find("200 OK") != std::string::npos)
    {
        mapped_ = true;
        mapped_port_ = internal_port_;
        return true;
    }

    return false;
}

bool UpnpManager::soap_delete_port_mapping()
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (igd_control_url_.empty()) return false;

    std::string ext_port = std::to_string(mapped_port_ ? mapped_port_ : internal_port_);

    std::string body =
        "<?xml version=\"1.0\"?>\r\n"
        "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
        "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">\r\n"
        "  <s:Body>\r\n"
        "    <u:DeletePortMapping xmlns:u=\"" + igd_service_type_ + "\">\r\n"
        "      <NewRemoteHost></NewRemoteHost>\r\n"
        "      <NewExternalPort>" + ext_port + "</NewExternalPort>\r\n"
        "      <NewProtocol>TCP</NewProtocol>\r\n"
        "    </u:DeletePortMapping>\r\n"
        "  </s:Body>\r\n"
        "</s:Envelope>\r\n";

    std::string action = igd_service_type_ + "#DeletePortMapping";
    http_request(igd_control_url_, action, body);
    return true;
}

bool UpnpManager::soap_get_external_ip()
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (igd_control_url_.empty()) return false;

    std::string body =
        "<?xml version=\"1.0\"?>\r\n"
        "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
        "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">\r\n"
        "  <s:Body>\r\n"
        "    <u:GetExternalIPAddress xmlns:u=\"" + igd_service_type_ + "\">\r\n"
        "    </u:GetExternalIPAddress>\r\n"
        "  </s:Body>\r\n"
        "</s:Envelope>\r\n";

    std::string action = igd_service_type_ + "#GetExternalIPAddress";
    std::string response = http_request(igd_control_url_, action, body);

    if (response.empty()) return false;

    // Extract the IP from the SOAP response
    size_t ip_start = response.find("<NewExternalIPAddress>");
    if (ip_start == std::string::npos) return false;

    ip_start += 22;
    size_t ip_end = response.find("</NewExternalIPAddress>", ip_start);
    if (ip_end == std::string::npos) return false;

    external_ip_ = response.substr(ip_start, ip_end - ip_start);
    return !external_ip_.empty();
}

// ===== NAT-PMP / PCP =====

void UpnpManager::try_nat_pmp()
{
    gateway_ip_ = get_local_gateway();
    if (gateway_ip_.empty()) return;

    nat_pmp_get_external_ip();
    nat_pmp_map_port();
}

bool UpnpManager::nat_pmp_get_external_ip()
{
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) return false;

    struct sockaddr_in gateway;
    std::memset(&gateway, 0, sizeof(gateway));
    gateway.sin_family = AF_INET;
    gateway.sin_port = htons(5351);
    inet_pton(AF_INET, gateway_ip_.c_str(), &gateway.sin_addr);

    // NAT-PMP external address request: version(1) opcode(0)
    uint8_t request[2] = {0, 0};

    if (sendto(sock, (const char *)request, 2, 0,
               (struct sockaddr *)&gateway, sizeof(gateway)) < 0)
    {
        closesocket(sock);
        return false;
    }

#ifdef _WIN32
    DWORD timeout = 3000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout, sizeof(timeout));
#else
    struct timeval tv;
    tv.tv_sec = 3;
    tv.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif

    uint8_t response[16];
    struct sockaddr_in from;
    int fromlen = sizeof(from);
    int n = recvfrom(sock, (char *)response, sizeof(response), 0,
                     (struct sockaddr *)&from, &fromlen);
    closesocket(sock);

    if (n < 12) return false;
    if (response[0] != 0 || response[1] != 128) return false; // version=0, opcode=128(=response)

    // Response: version(1) opcode(1) result(2) epoch(4) external_ip(4)
    uint32_t ext_ip = (response[8] << 24) | (response[9] << 16) | (response[10] << 8) | response[11];

    char ip_str[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &ext_ip, ip_str, sizeof(ip_str));

    std::lock_guard<std::mutex> lock(mutex_);
    external_ip_ = ip_str;
    nat_pmp_available_ = true;
    return true;
}

bool UpnpManager::nat_pmp_map_port()
{
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) return false;

    struct sockaddr_in gateway;
    std::memset(&gateway, 0, sizeof(gateway));
    gateway.sin_family = AF_INET;
    gateway.sin_port = htons(5351);
    inet_pton(AF_INET, gateway_ip_.c_str(), &gateway.sin_addr);

    std::string local_ip = get_local_ip();
    uint32_t int_ip = 0;
    inet_pton(AF_INET, local_ip.c_str(), &int_ip);

    // NAT-PMP port mapping request:
    // version(1) opcode(1) reserved(2) internal_port(2) external_port(2) lifetime(4)
    uint8_t request[12];
    request[0] = 0;  // version
    request[1] = 1;  // opcode: TCP
    request[2] = 0;  // reserved
    request[3] = 0;  // reserved
    request[4] = (internal_port_ >> 8) & 0xFF;
    request[5] = internal_port_ & 0xFF;
    request[6] = 0;  // suggested external port (0 = let router choose)
    request[7] = 0;
    uint32_t lifetime = 3600;
    request[8] = (lifetime >> 24) & 0xFF;
    request[9] = (lifetime >> 16) & 0xFF;
    request[10] = (lifetime >> 8) & 0xFF;
    request[11] = lifetime & 0xFF;

    if (sendto(sock, (const char *)request, 12, 0,
               (struct sockaddr *)&gateway, sizeof(gateway)) < 0)
    {
        closesocket(sock);
        return false;
    }

#ifdef _WIN32
    DWORD timeout = 3000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout, sizeof(timeout));
#else
    struct timeval tv;
    tv.tv_sec = 3;
    tv.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif

    uint8_t response[16];
    struct sockaddr_in from;
    int fromlen = sizeof(from);
    int n = recvfrom(sock, (char *)response, sizeof(response), 0,
                     (struct sockaddr *)&from, &fromlen);
    closesocket(sock);

    if (n < 16) return false;
    if (response[0] != 0 || response[1] != 129) return false; // version=0, opcode=129

    // Result code: 0 = success
    uint16_t result = (response[2] << 8) | response[3];
    if (result != 0) return false;

    // Extract mapped external port
    uint16_t ext_port = (response[8] << 8) | response[9];
    uint32_t lifetime_resp = (response[12] << 24) | (response[13] << 16) | (response[14] << 8) | response[15];

    std::lock_guard<std::mutex> lock(mutex_);
    mapped_port_ = ext_port;
    nat_pmp_lifetime_ = lifetime_resp;
    mapped_ = true;
    return true;
}

// ===== Utility: Local IP / Gateway =====

std::string UpnpManager::get_local_ip()
{
    // Create a UDP socket to determine local IP (no actual connection)
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) return "";

    struct sockaddr_in dest;
    std::memset(&dest, 0, sizeof(dest));
    dest.sin_family = AF_INET;
    dest.sin_port = htons(80);
    inet_pton(AF_INET, "8.8.8.8", &dest.sin_addr);

    // Connect UDP socket to determine local IP (doesn't send data)
    if (connect(sock, (struct sockaddr *)&dest, sizeof(dest)) < 0)
    {
        closesocket(sock);
        return "";
    }

    struct sockaddr_in local;
    int local_len = sizeof(local);
    if (getsockname(sock, (struct sockaddr *)&local, &local_len) < 0)
    {
        closesocket(sock);
        return "";
    }

    closesocket(sock);

    char ip_str[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &local.sin_addr, ip_str, sizeof(ip_str));
    return ip_str;
}

std::string UpnpManager::get_local_gateway()
{
#ifdef _WIN32
    // Use GetAdaptersInfo to find the default gateway
    ULONG buf_len = 0;
    if (GetAdaptersInfo(nullptr, &buf_len) != ERROR_BUFFER_OVERFLOW)
        return "";

    auto *adapters = static_cast<IP_ADAPTER_INFO *>(malloc(buf_len));
    if (GetAdaptersInfo(adapters, &buf_len) != NO_ERROR)
    {
        free(adapters);
        return "";
    }

    std::string gateway;
    for (auto *adapter = adapters; adapter; adapter = adapter->Next)
    {
        if (adapter->GatewayList.IpAddress.String[0] != '\0')
        {
            gateway = adapter->GatewayList.IpAddress.String;
            break;
        }
    }
    free(adapters);
    return gateway;
#else
    // Read /proc/net/route to find default gateway
    FILE *f = fopen("/proc/net/route", "r");
    if (!f) return "";

    char line[256];
    while (fgets(line, sizeof(line), f))
    {
        // Skip header
        if (line[0] != '\t' && line[0] != ' ') continue;

        unsigned int dest = 0, gw = 0;
        char iface[64];
        if (sscanf(line, "%63s %x %x", iface, &dest, &gw) == 3)
        {
            if (dest == 0) // default route
            {
                struct in_addr addr;
                addr.s_addr = gw;
                std::string gateway_ip = inet_ntoa(addr);
                fclose(f);
                return gateway_ip;
            }
        }
    }
    fclose(f);
    return "";
#endif
}

} // namespace yuan::net::bit_torrent
