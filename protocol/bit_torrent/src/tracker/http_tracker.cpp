#include "tracker/http_tracker.h"
#include "http_client.h"
#include "request.h"
#include "response.h"
#include "utils.h"

#include <memory>
#include <sstream>
#include <thread>
#include <cstring>

namespace yuan::net::bit_torrent
{

    namespace
    {

        bool split_http_url(const std::string &url, std::string &authority, std::string &request_target)
        {
            const auto scheme_pos = url.find("://");
            if (scheme_pos == std::string::npos) {
                return false;
            }

            const auto scheme = url.substr(0, scheme_pos);
            if (scheme != "http" && scheme != "https") {
                return false;
            }

            const auto authority_begin = scheme_pos + 3;
            const auto path_pos = url.find('/', authority_begin);
            if (path_pos == std::string::npos) {
                authority = url.substr(authority_begin);
                request_target = "/";
                return !authority.empty();
            }

            authority = url.substr(authority_begin, path_pos - authority_begin);
            request_target = url.substr(path_pos);
            return !authority.empty() && !request_target.empty();
        }

        bool tracker_response_ok(const TrackerResponse &response)
        {
            return !response.peers_.empty() || response.interval_ > 0;
        }

        void configure_announce_request(http::HttpRequest *req,
                                        const std::string &request_target,
                                        const std::string &authority)
        {
            req->set_method(http::HttpMethod::get_);
            req->set_raw_url(request_target);
            req->add_header("Connection", "close");
            req->add_header("Host", authority);
            req->add_header("User-Agent", "YuanBT/1.0");
            req->send();
        }

    } // namespace

    HttpTracker::HttpTracker()
        : peer_id_(generate_peer_id())
    {
    }

    HttpTracker::~HttpTracker()
    {
        std::vector<std::thread> to_join;
        {
            std::lock_guard<std::mutex> lock(workers_mutex_);
            to_join = std::move(workers_);
        }
        for (auto &t : to_join) {
            if (t.joinable())
                t.join();
        }
    }

    std::string HttpTracker::build_announce_url(const std::string & tracker_url,
                                                const TorrentMeta & meta,
                                                int32_t port,
                                               int64_t uploaded,
                                               int64_t downloaded,
                                               int64_t left,
                                               TrackerAnnounceEvent event,
                                               const std::string &peer_id)
    {
        // info_hash: each byte URL-encoded (%XX)
        std::string info_hash_encoded;
        info_hash_encoded.reserve(meta.info_hash_.size() * 3);
        for (uint8_t b : meta.info_hash_) {
            char buf[4];
            snprintf(buf, sizeof(buf), "%%%.2X", b);
            info_hash_encoded += buf;
        }

        std::string peer_id_encoded = url_encode(peer_id.empty() ? peer_id_ : peer_id);

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
        url += "&compact=1"; // compact format (binary peers)
        url += "&no_peer_id=1";
        switch (event) {
        case TrackerAnnounceEvent::completed:
            url += "&event=completed";
            break;
        case TrackerAnnounceEvent::started:
            url += "&event=started";
            break;
        case TrackerAnnounceEvent::stopped:
            url += "&event=stopped";
            break;
        case TrackerAnnounceEvent::none:
        default:
            break;
        }

        if (!last_tracker_id_.empty())
            url += "&trackerid=" + url_encode(last_tracker_id_);

        return url;
    }

    bool HttpTracker::parse_response(const std::string & body, TrackerResponse & out)
    {
        // Response is bencoded dictionary:
        // d8:completei<seeders>e10:incompletei<leechers>e8:intervali<sec>e5:peers6:<binary>e
        auto *data = BencodingDataConverter::parse(body);
        if (!data || data->type_ != DataType::dictionary_) {
            if (data)
                delete data;
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

        if (auto *v = dict->get_val("peers"); v && v->type_ == DataType::string_) {
            // Compact format: 6 bytes per peer (4-byte IP + 2-byte port, big-endian)
            const auto &peers_str = dynamic_cast<StringData *>(v)->get_data();
            if (peers_str.size() >= 6) {
                for (size_t i = 0; i + 6 <= peers_str.size(); i += 6) {
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
        } else if (auto *v = dict->get_val("peers"); v && v->type_ == DataType::list_) {
            // Non-compact format: list of dictionaries
            auto *peers_list = dynamic_cast<Listdata *>(v);
            for (auto *peer_node : peers_list->get_data()) {
                if (!peer_node || peer_node->type_ != DataType::dictionary_)
                    continue;
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

    bool HttpTracker::announce(const std::string & tracker_url,
                               const TorrentMeta & meta,
                               int32_t port,
                               int64_t uploaded,
                               int64_t downloaded,
                               int64_t left,
                               TrackerAnnounceEvent event,
                               TrackerResponse * out,
                               const std::string &peer_id)
    {
        const std::string url = build_announce_url(tracker_url, meta, port, uploaded, downloaded, left, event, peer_id);

        std::string authority;
        std::string request_target;
        if (!split_http_url(url, authority, request_target)) {
            return false;
        }

        auto client = std::make_unique<http::HttpClient>();
        if (!client->query(url)) {
            return false;
        }

        std::string body;
        const bool request_ok = client->connect(
            [request_target, authority](http::HttpRequest *req) {
            configure_announce_request(req, request_target, authority);
            },
            [&body](http::HttpRequest *, http::HttpResponse *response) {
            if (!response || response->get_body_length() == 0) {
                return;
            }

            const char *begin = response->body_begin();
            if (!begin) {
                return;
            }

            body.assign(begin, response->get_body_length());
            });

        if (!request_ok || body.empty()) {
            return false;
        }

        TrackerResponse resp;
        const bool ok = parse_response(body, resp);
        if (ok && out)
            *out = resp;
        return ok;
    }

    void HttpTracker::announce_async(const std::string & tracker_url,
                                     const TorrentMeta & meta,
                                     int32_t port,
                                     TrackerResponseHandler handler,
                                     int64_t uploaded,
                                     int64_t downloaded,
                                     int64_t left,
                                     TrackerAnnounceEvent event)
    {
        std::thread t([
            this,
            tracker_url,
            meta,
            port,
            handler = std::move(handler),
            uploaded,
            downloaded,
            left,
            event
        ]() mutable {
        TrackerResponse resp;
        const bool ok = announce(tracker_url, meta, port, uploaded, downloaded, left, event, &resp);
        if (handler)
        {
            handler(ok ? resp : TrackerResponse{});
        } });
        {
            std::lock_guard<std::mutex> lock(workers_mutex_);
            workers_.push_back(std::move(t));
        }
    }

    std::string HttpTracker::build_scrape_url(const std::string & tracker_url,
                                              const TorrentMeta & meta)
    {
        std::string scrape_url = tracker_url;

        auto announce_pos = scrape_url.rfind("/announce");
        if (announce_pos != std::string::npos) {
            scrape_url = scrape_url.substr(0, announce_pos) + "/scrape";
        } else {
            auto last_slash = scrape_url.rfind('/');
            if (last_slash != std::string::npos) {
                auto scheme_pos = scrape_url.find("://");
                if (scheme_pos != std::string::npos && last_slash > scheme_pos + 2) {
                    scrape_url = scrape_url.substr(0, last_slash) + "/scrape";
                }
            }
        }

        std::string info_hash_encoded;
        info_hash_encoded.reserve(meta.info_hash_.size() * 3);
        for (uint8_t b : meta.info_hash_) {
            char buf[4];
            snprintf(buf, sizeof(buf), "%%%.2X", b);
            info_hash_encoded += buf;
        }

        if (scrape_url.find('?') == std::string::npos)
            scrape_url += "?";
        else
            scrape_url += "&";

        scrape_url += "info_hash=" + info_hash_encoded;
        return scrape_url;
    }

    bool HttpTracker::parse_scrape_response(const std::string & body, const std::vector<uint8_t> & info_hash, ScrapeResponse & out)
    {
        auto *data = BencodingDataConverter::parse(body);
        if (!data || data->type_ != DataType::dictionary_) {
            if (data)
                delete data;
            return false;
        }

        auto *dict = dynamic_cast<DicttionaryData *>(data);

        auto *files = dict->get_val("files");
        if (files && files->type_ == DataType::dictionary_) {
            auto *files_dict = dynamic_cast<DicttionaryData *>(files);
            for (auto & [
                            key,
                            val
                        ] : files_dict->get_data()) {
                if (!val || val->type_ != DataType::dictionary_)
                    continue;

                if (key.size() != 20 || info_hash.size() != 20)
                    continue;

                bool match = true;
                for (size_t i = 0; i < 20; i++) {
                    if (static_cast<uint8_t>(key[i]) != info_hash[i]) {
                        match = false;
                        break;
                    }
                }
                if (!match)
                    continue;

                auto *entry = dynamic_cast<DicttionaryData *>(val);
                if (auto *v = entry->get_val("complete"); v && v->type_ == DataType::integer_)
                    out.complete_ = dynamic_cast<IntegerData *>(v)->get_data();
                if (auto *v = entry->get_val("downloaded"); v && v->type_ == DataType::integer_)
                    out.downloaded_ = dynamic_cast<IntegerData *>(v)->get_data();
                if (auto *v = entry->get_val("incomplete"); v && v->type_ == DataType::integer_)
                    out.incomplete_ = dynamic_cast<IntegerData *>(v)->get_data();
                break;
            }
        }

        if (auto *v = dict->get_val("failure reason"); v && v->type_ == DataType::string_) {
            delete data;
            return false;
        }

        delete data;
        return true;
    }

    bool HttpTracker::scrape(const std::string & tracker_url,
                             const TorrentMeta & meta,
                             ScrapeResponse * out)
    {
        const std::string url = build_scrape_url(tracker_url, meta);

        std::string authority;
        std::string request_target;
        if (!split_http_url(url, authority, request_target)) {
            return false;
        }

        auto client = std::make_unique<http::HttpClient>();
        if (!client->query(url)) {
            return false;
        }

        std::string body;
        const bool request_ok = client->connect(
            [request_target, authority](http::HttpRequest *req) {
            configure_announce_request(req, request_target, authority);
            },
            [&body](http::HttpRequest *, http::HttpResponse *response) {
            if (!response || response->get_body_length() == 0) {
                return;
            }

            const char *begin = response->body_begin();
            if (!begin) {
                return;
            }

            body.assign(begin, response->get_body_length());
            });

        if (!request_ok || body.empty()) {
            return false;
        }

        ScrapeResponse resp;
        const bool ok = parse_scrape_response(body, meta.info_hash_, resp);
        if (ok && out)
            *out = resp;
        return ok;
    }

    void HttpTracker::scrape_async(const std::string & tracker_url,
                                   const TorrentMeta & meta,
                                   ScrapeResponseHandler handler)
    {
        std::thread t([
            this,
            tracker_url,
            meta,
            handler = std::move(handler)
        ]() mutable {
        ScrapeResponse resp;
        scrape(tracker_url, meta, &resp);
        if (handler)
        {
            handler(resp);
        } });
        {
            std::lock_guard<std::mutex> lock(workers_mutex_);
            workers_.push_back(std::move(t));
        }
    }

} // namespace yuan::net::bit_torrent
