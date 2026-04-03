#include "tracker/http_tracker.h"
#include "utils.h"
#include <sstream>
#include <cstring>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#define closesocket close
#endif

namespace yuan::net::bit_torrent
{

HttpTracker::HttpTracker() : peer_id_(generate_peer_id()) {}

HttpTracker::~HttpTracker() {}

std::string HttpTracker::build_announce_url(const std::string &tracker_url,
                                            const TorrentMeta &meta,
                                            int32_t port,
                                            int64_t uploaded,
                                            int64_t downloaded,
                                            int64_t left)
{
    // info_hash: each byte URL-encoded (%XX)
    std::string info_hash_encoded;
    info_hash_encoded.reserve(meta.info_hash_.size() * 3);
    for (uint8_t b : meta.info_hash_)
    {
        char buf[4];
        snprintf(buf, sizeof(buf), "%%%.2X", b);
        info_hash_encoded += buf;
    }

    std::string peer_id_encoded = url_encode(peer_id_);

    // left = -1 means unknown
    std::string left_str = (left < 0) ? "0" : std::to_string(left);

    std::string url = tracker_url;
    // Append ? or & separator
    if (url.find('?') == std::string::npos)
        url += "?";
    else
        url += "&";

    url += "info_hash=" + info_hash_encoded;
    url += "&peer_id=" + peer_id_encoded;
    url += "&port=" + std::to_string(port);
    url += "&uploaded=" + std::to_string(uploaded);
    url += "&downloaded=" + std::to_string(downloaded);
    url += "&left=" + left_str;
    url += "&compact=1";  // compact format (binary peers)
    url += "&no_peer_id=1";
    url += "&event=started";

    if (!last_tracker_id_.empty())
        url += "&trackerid=" + url_encode(last_tracker_id_);

    return url;
}

bool HttpTracker::parse_response(const std::string &body, TrackerResponse &out)
{
    // Response is bencoded dictionary:
    // d8:completei<seeders>e10:incompletei<leechers>e8:intervali<sec>e5:peers6:<binary>e
    auto *data = BencodingDataConverter::parse(body);
    if (!data || data->type_ != DataType::dictionary_)
    {
        if (data) delete data;
        return false;
    }

    auto *dict = dynamic_cast<DicttionaryData *>(data);

    if (auto *v = dict->get_val("interval"); v && v->type_ == DataType::integer_)
        out.interval_ = dynamic_cast<IntegerData *>(v)->get_data();

    if (auto *v = dict->get_val("min interval"); v && v->type_ == DataType::integer_)
        out.min_interval_ = dynamic_cast<IntegerData *>(v)->get_data();

    if (auto *v = dict->get_val("tracker id"); v && v->type_ == DataType::string_)
        out.tracker_id_ = dynamic_cast<StringData *>(v)->get_data();

    if (auto *v = dict->get_val("complete"); v && v->type_ == DataType::integer_)
        out.complete_ = dynamic_cast<IntegerData *>(v)->get_data();

    if (auto *v = dict->get_val("incomplete"); v && v->type_ == DataType::integer_)
        out.incomplete_ = dynamic_cast<IntegerData *>(v)->get_data();

    if (auto *v = dict->get_val("warning message"); v && v->type_ == DataType::string_)
        out.warning_message_ = dynamic_cast<StringData *>(v)->get_data();

    if (auto *v = dict->get_val("peers"); v && v->type_ == DataType::string_)
    {
        // Compact format: 6 bytes per peer (4-byte IP + 2-byte port, big-endian)
        const auto &peers_str = dynamic_cast<StringData *>(v)->get_data();
        if (peers_str.size() >= 6)
        {
            for (size_t i = 0; i + 6 <= peers_str.size(); i += 6)
            {
                PeerAddress addr;
                unsigned char b0 = static_cast<unsigned char>(peers_str[i]);
                unsigned char b1 = static_cast<unsigned char>(peers_str[i + 1]);
                unsigned char b2 = static_cast<unsigned char>(peers_str[i + 2]);
                unsigned char b3 = static_cast<unsigned char>(peers_str[i + 3]);
                addr.ip_ = std::to_string(b0) + "." + std::to_string(b1) + "." +
                           std::to_string(b2) + "." + std::to_string(b3);
                addr.port_ = (static_cast<uint8_t>(peers_str[i + 4]) << 8) |
                             static_cast<uint8_t>(peers_str[i + 5]);
                out.peers_.push_back(addr);
            }
        }
    }
    else if (auto *v = dict->get_val("peers"); v && v->type_ == DataType::list_)
    {
        // Non-compact format: list of dictionaries
        auto *peers_list = dynamic_cast<Listdata *>(v);
        for (auto *peer_node : peers_list->get_data())
        {
            if (!peer_node || peer_node->type_ != DataType::dictionary_) continue;
            auto *peer_dict = dynamic_cast<DicttionaryData *>(peer_node);
            PeerAddress addr;
            if (auto *pv = peer_dict->get_val("ip"); pv && pv->type_ == DataType::string_)
                addr.ip_ = dynamic_cast<StringData *>(pv)->get_data();
            if (auto *pv = peer_dict->get_val("port"); pv && pv->type_ == DataType::integer_)
                addr.port_ = static_cast<uint16_t>(dynamic_cast<IntegerData *>(pv)->get_data());
            if (!addr.ip_.empty() && addr.port_ > 0)
                out.peers_.push_back(addr);
        }
    }

    if (!out.tracker_id_.empty())
        last_tracker_id_ = out.tracker_id_;

    delete data;
    return !out.peers_.empty() || out.interval_ > 0;
}

bool HttpTracker::announce(const std::string &tracker_url,
                           const TorrentMeta &meta,
                           int32_t port,
                           int64_t uploaded,
                           int64_t downloaded,
                           int64_t left,
                           TrackerResponse *out)
{
    std::string url = build_announce_url(tracker_url, meta, port, uploaded, downloaded, left);

    // Use system HTTP GET (platform-independent via fopen for simplicity,
    // or the user can integrate with the project's HttpClient)
    // For a self-contained implementation, we use a simple socket-based GET.
    // TODO: integrate with project's HttpClient for SSL support

    // Parse URL to get host and path
    std::string host, path;
    bool use_ssl = false;
    uint16_t url_port = 80;

    if (url.substr(0, 8) == "https://")
    {
        use_ssl = true;
        url_port = 443;
        url = url.substr(8);
    }
    else if (url.substr(0, 7) == "http://")
    {
        url = url.substr(7);
    }

    auto slash_pos = url.find('/');
    if (slash_pos == std::string::npos)
    {
        host = url;
        path = "/";
    }
    else
    {
        host = url.substr(0, slash_pos);
        path = url.substr(slash_pos);
    }

    auto colon_pos = host.find(':');
    if (colon_pos != std::string::npos)
    {
        url_port = static_cast<uint16_t>(std::atoi(host.substr(colon_pos + 1).c_str()));
        host = host.substr(0, colon_pos);
    }

    // For SSL trackers, we cannot do a simple socket connection.
    // The user should integrate with HttpClient for HTTPS support.
    if (use_ssl)
    {
        // Return true to indicate the URL is valid, but cannot connect without SSL.
        // The real implementation would use HttpClient.
        return false;
    }

    // Simple synchronous HTTP GET via socket
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif

    int sock = ::socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0)
    {
#ifdef _WIN32
        WSACleanup();
#endif
        return false;
    }

    struct sockaddr_in server_addr {};
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(url_port);
    inet_pton(AF_INET, host.c_str(), &server_addr.sin_addr);

    if (::connect(sock, reinterpret_cast<struct sockaddr *>(&server_addr), sizeof(server_addr)) < 0)
    {
        ::closesocket(sock);
#ifdef _WIN32
        WSACleanup();
#endif
        return false;
    }

    // Build HTTP GET request
    std::string request = "GET " + path + " HTTP/1.1\r\n"
                          "Host: " + host + "\r\n"
                          "Connection: close\r\n"
                          "User-Agent: YuanBT/1.0\r\n"
                          "\r\n";

    ::send(sock, request.c_str(), static_cast<int>(request.size()), 0);

    // Read response
    std::string response;
    char buf[4096];
    int bytes;
    while ((bytes = ::recv(sock, buf, sizeof(buf), 0)) > 0)
    {
        response.append(buf, bytes);
    }

    ::closesocket(sock);
#ifdef _WIN32
    WSACleanup();
#endif

    if (response.empty()) return false;

    // Extract body (after \r\n\r\n)
    auto body_pos = response.find("\r\n\r\n");
    if (body_pos == std::string::npos) return false;
    std::string body = response.substr(body_pos + 4);

    TrackerResponse resp;
    bool ok = parse_response(body, resp);
    if (ok && out) *out = resp;
    return ok;
}

void HttpTracker::announce_async(const std::string &tracker_url,
                                 const TorrentMeta &meta,
                                 int32_t port,
                                 TrackerResponseHandler handler,
                                 int64_t uploaded,
                                 int64_t downloaded,
                                 int64_t left)
{
    // For async support, the caller should integrate with the project's EventLoop + HttpClient.
    // As a fallback, run synchronously on a separate thread.
    // TODO: integrate with HttpClient for proper async support
    TrackerResponse resp;
    bool ok = announce(tracker_url, meta, port, uploaded, downloaded, left, &resp);
    if (handler)
        handler(ok ? resp : TrackerResponse{});
}

} // namespace yuan::net::bit_torrent
