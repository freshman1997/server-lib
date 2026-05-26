#include "rtsp_server.h"

#include "buffer/byte_buffer.h"
#include "base/utils/base64.h"
#include "logger.h"
#include "net/socket/socket_ops.h"
#include "rtsp_parser.h"
#include "rtsp_framing.h"
#include "rtsp_sdp.h"
#include "rtsp_session.h"
#include "rtsp_transport.h"

#include "openssl/evp.h"
#include "openssl/crypto.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <chrono>
#include <functional>
#include <iomanip>
#include <limits>
#include <memory>
#include <random>
#include <sstream>
#include <string>
#include <string_view>
#include <map>
#include <deque>
#include <vector>
#include <utility>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <sys/socket.h>
#include <sys/types.h>
#endif

namespace yuan::net::rtsp
{

namespace
{

std::string generate_session_id()
{
    static thread_local std::mt19937_64 rng{std::random_device{}()};
    std::uniform_int_distribution<uint64_t> dist(1, std::numeric_limits<uint64_t>::max());
    std::ostringstream oss;
    oss << std::hex << std::uppercase << dist(rng);
    return oss.str();
}

uint64_t now_ms()
{
    using clock = std::chrono::steady_clock;
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        clock::now().time_since_epoch()).count());
}

uint64_t parse_timeout_ms_from_session_header(const RtspRequest &request)
{
    const std::string *value = request.header("Session");
    if (!value || value->empty()) {
        return 60000;
    }

    const auto timeout_pos = value->find("timeout=");
    if (timeout_pos == std::string::npos) {
        return 60000;
    }

    const std::string timeout_text = value->substr(timeout_pos + 8);
    try {
        const int seconds = std::stoi(timeout_text);
        if (seconds <= 0) {
            return 60000;
        }
        return static_cast<uint64_t>(seconds) * 1000;
    } catch (...) {
        return 60000;
    }
}

bool parse_keepalive_timeout_ms(const RtspRequest &request, uint64_t &out_timeout_ms)
{
    const std::string *content_type = request.header("Content-Type");
    if (!content_type || *content_type != "text/parameters") {
        return false;
    }

    const std::string &body = request.body;
    const std::string key = "timeout=";
    const auto pos = body.find(key);
    if (pos == std::string::npos) {
        return false;
    }

    const auto end = body.find_first_of("\r\n", pos);
    const std::string text = body.substr(pos + key.size(), end == std::string::npos ? std::string::npos : (end - (pos + key.size())));
    try {
        const int seconds = std::stoi(text);
        if (seconds <= 0) {
            return false;
        }
        out_timeout_ms = static_cast<uint64_t>(seconds) * 1000;
        return true;
    } catch (...) {
        return false;
    }
}

bool parse_scale_header(const RtspRequest &request, double &out_scale)
{
    const std::string *scale_text = request.header("Scale");
    if (!scale_text) {
        return true;
    }
    try {
        const double scale = std::stod(*scale_text);
        if (scale <= 0.0) {
            return false;
        }
        out_scale = scale;
        return true;
    } catch (...) {
        return false;
    }
}

bool validate_client_ports(const RtspTransportSpec &spec)
{
    if (spec.transport != RtspLowerTransport::rtp_avp_udp) {
        return true;
    }

    if (spec.client_rtp_port < 0 || spec.client_rtcp_port < 0) {
        return true;
    }

    if ((spec.client_rtp_port % 2) != 0) {
        return false;
    }
    if (spec.client_rtcp_port != spec.client_rtp_port + 1) {
        return false;
    }
    if (spec.client_rtp_port > 65535 || spec.client_rtcp_port > 65535) {
        return false;
    }
    return true;
}

bool validate_interleaved_channels(const RtspTransportSpec &spec)
{
    if (spec.transport != RtspLowerTransport::rtp_avp_tcp) {
        return true;
    }
    if (spec.interleaved_rtp_channel < 0 && spec.interleaved_rtcp_channel < 0) {
        return true;
    }
    if (spec.interleaved_rtp_channel < 0 || spec.interleaved_rtcp_channel < 0) {
        return false;
    }
    if (spec.interleaved_rtp_channel == spec.interleaved_rtcp_channel) {
        return false;
    }
    if (spec.interleaved_rtp_channel > 255 || spec.interleaved_rtcp_channel > 255) {
        return false;
    }
    return true;
}

bool supports_transport_candidate(const RtspTransportSpec &spec)
{
    return spec.unicast;
}

bool starts_with(const std::string &value, const std::string &prefix)
{
    return value.size() >= prefix.size() && value.compare(0, prefix.size(), prefix) == 0;
}

template <typename T>
void erase_prefixed_keys(std::map<std::string, T> &table, const std::string &prefix)
{
    for (auto it = table.begin(); it != table.end();) {
        if (starts_with(it->first, prefix)) {
            it = table.erase(it);
        } else {
            ++it;
        }
    }
}

std::string escape_header_quoted(const std::string &text)
{
    std::string out;
    out.reserve(text.size());
    for (char ch : text) {
        if (ch == '"' || ch == '\\') {
            out.push_back('\\');
        }
        out.push_back(ch);
    }
    return out;
}

bool parse_basic_credentials(const std::string &header, std::string &out_user, std::string &out_pass)
{
    if (!starts_with(header, "Basic ")) {
        return false;
    }
    const std::string encoded = header.substr(6);
    const std::string decoded = ::yuan::base::util::base64_decode(encoded);
    const auto sep = decoded.find(':');
    if (sep == std::string::npos) {
        return false;
    }
    out_user = decoded.substr(0, sep);
    out_pass = decoded.substr(sep + 1);
    return !out_user.empty();
}

char ascii_tolower(char ch)
{
    return static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
}

bool iequals(const std::string &lhs, const std::string &rhs)
{
    if (lhs.size() != rhs.size()) {
        return false;
    }
    for (std::size_t i = 0; i < lhs.size(); ++i) {
        if (ascii_tolower(lhs[i]) != ascii_tolower(rhs[i])) {
            return false;
        }
    }
    return true;
}

bool istarts_with(const std::string &value, const std::string &prefix)
{
    if (value.size() < prefix.size()) {
        return false;
    }
    for (std::size_t i = 0; i < prefix.size(); ++i) {
        if (ascii_tolower(value[i]) != ascii_tolower(prefix[i])) {
            return false;
        }
    }
    return true;
}

std::string trim_copy(const std::string &text)
{
    std::size_t begin = 0;
    while (begin < text.size() && std::isspace(static_cast<unsigned char>(text[begin]))) {
        ++begin;
    }
    std::size_t end = text.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1]))) {
        --end;
    }
    return text.substr(begin, end - begin);
}

bool parse_ipv4_octet(const std::string &text, std::size_t begin, std::size_t end, uint32_t &out)
{
    if (begin >= end) {
        return false;
    }
    uint32_t value = 0;
    for (std::size_t i = begin; i < end; ++i) {
        const char ch = text[i];
        if (ch < '0' || ch > '9') {
            return false;
        }
        value = value * 10 + static_cast<uint32_t>(ch - '0');
        if (value > 255u) {
            return false;
        }
    }
    out = value;
    return true;
}

bool parse_ipv4_address(const std::string &ip, uint32_t &out_ip)
{
    std::array<uint32_t, 4> octets{0, 0, 0, 0};
    std::size_t start = 0;
    int part = 0;
    for (std::size_t i = 0; i <= ip.size(); ++i) {
        if (i == ip.size() || ip[i] == '.') {
            if (part >= 4 || !parse_ipv4_octet(ip, start, i, octets[static_cast<std::size_t>(part)])) {
                return false;
            }
            ++part;
            start = i + 1;
        }
    }
    if (part != 4) {
        return false;
    }
    out_ip = (octets[0] << 24) | (octets[1] << 16) | (octets[2] << 8) | octets[3];
    return true;
}

bool parse_cidr_v4(const std::string &cidr, uint32_t &out_network, uint8_t &out_prefix)
{
    const auto slash = cidr.find('/');
    if (slash == std::string::npos) {
        return false;
    }
    const std::string ip_part = trim_copy(cidr.substr(0, slash));
    const std::string prefix_part = trim_copy(cidr.substr(slash + 1));
    if (!parse_ipv4_address(ip_part, out_network)) {
        return false;
    }
    try {
        std::size_t consumed = 0;
        const int prefix = std::stoi(prefix_part, &consumed);
        if (consumed != prefix_part.size() || prefix < 0 || prefix > 32) {
            return false;
        }
        out_prefix = static_cast<uint8_t>(prefix);
    } catch (...) {
        return false;
    }
    return true;
}

bool ip_in_cidr_v4(uint32_t ip, uint32_t network, uint8_t prefix)
{
    if (prefix == 0) {
        return true;
    }
    if (prefix >= 32) {
        return ip == network;
    }
    const uint32_t mask = 0xFFFFFFFFu << (32 - prefix);
    return (ip & mask) == (network & mask);
}

bool has_any_prefix_match(const std::vector<std::string> &prefixes, const std::string &uri)
{
    for (const std::string &prefix : prefixes) {
        if (!prefix.empty() && uri.rfind(prefix, 0) == 0) {
            return true;
        }
    }
    return false;
}

std::string digest_hex(std::string_view text, const EVP_MD *md)
{
    if (!md) {
        return {};
    }

    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    if (!ctx) {
        return {};
    }

    unsigned char digest[EVP_MAX_MD_SIZE] = {0};
    unsigned int digest_len = 0;
    bool ok = EVP_DigestInit_ex(ctx, md, nullptr) == 1 &&
              EVP_DigestUpdate(ctx, text.data(), text.size()) == 1 &&
              EVP_DigestFinal_ex(ctx, digest, &digest_len) == 1;
    EVP_MD_CTX_free(ctx);
    if (!ok) {
        return {};
    }

    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(digest_len * 2);
    for (unsigned int i = 0; i < digest_len; ++i) {
        const unsigned char b = digest[i];
        out.push_back(kHex[(b >> 4) & 0x0F]);
        out.push_back(kHex[b & 0x0F]);
    }
    return out;
}

std::string md5_hex(std::string_view text)
{
    return digest_hex(text, EVP_md5());
}

bool parse_digest_pairs(const std::string &text, std::map<std::string, std::string> &out)
{
    std::size_t i = 0;
    while (i < text.size()) {
        while (i < text.size() && (text[i] == ',' || std::isspace(static_cast<unsigned char>(text[i])))) {
            ++i;
        }
        if (i >= text.size()) {
            break;
        }

        const std::size_t key_begin = i;
        while (i < text.size() && text[i] != '=' && text[i] != ',') {
            ++i;
        }
        if (i >= text.size() || text[i] != '=') {
            return false;
        }
        std::string key = trim_copy(text.substr(key_begin, i - key_begin));
        ++i;

        std::string value;
        if (i < text.size() && text[i] == '"') {
            ++i;
            while (i < text.size()) {
                const char ch = text[i++];
                if (ch == '"') {
                    break;
                }
                if (ch == '\\' && i < text.size()) {
                    value.push_back(text[i++]);
                } else {
                    value.push_back(ch);
                }
            }
        } else {
            const std::size_t value_begin = i;
            while (i < text.size() && text[i] != ',') {
                ++i;
            }
            value = trim_copy(text.substr(value_begin, i - value_begin));
        }

        for (char &ch : key) {
            ch = ascii_tolower(ch);
        }
        if (!key.empty()) {
            out[key] = value;
        }
    }
    return !out.empty();
}

struct DigestAuthParams
{
    std::string username;
    std::string realm;
    std::string nonce;
    std::string uri;
    std::string response;
    std::string qop;
    std::string nc;
    std::string cnonce;
    std::string algorithm;
    std::string opaque;
};

bool parse_digest_credentials(const std::string &header, DigestAuthParams &out)
{
    if (!istarts_with(header, "Digest ")) {
        return false;
    }
    std::map<std::string, std::string> pairs;
    if (!parse_digest_pairs(header.substr(7), pairs)) {
        return false;
    }

    auto get = [&pairs](const char *key) -> std::string {
        auto iter = pairs.find(key);
        return iter == pairs.end() ? std::string{} : iter->second;
    };

    out.username = get("username");
    out.realm = get("realm");
    out.nonce = get("nonce");
    out.uri = get("uri");
    out.response = get("response");
    out.qop = get("qop");
    out.nc = get("nc");
    out.cnonce = get("cnonce");
    out.algorithm = get("algorithm");
    out.opaque = get("opaque");
    return !out.username.empty() && !out.realm.empty() && !out.nonce.empty() &&
           !out.uri.empty() && !out.response.empty();
}

std::string join_challenges(const std::vector<std::string> &challenges)
{
    std::string out;
    for (std::size_t i = 0; i < challenges.size(); ++i) {
        if (i > 0) {
            out += ", ";
        }
        out += challenges[i];
    }
    return out;
}

std::string generate_auth_nonce(uint64_t now)
{
    std::ostringstream oss;
    oss << std::hex << std::uppercase << now << '-' << generate_session_id();
    return md5_hex(oss.str());
}

bool equals_constant_time(const std::string &lhs, const std::string &rhs)
{
    if (lhs.size() != rhs.size()) {
        return false;
    }
    return CRYPTO_memcmp(lhs.data(), rhs.data(), lhs.size()) == 0;
}

bool is_digest_qop_auth(const std::string &qop)
{
    return qop.empty() || iequals(qop, "auth");
}

bool parse_hex_u64(std::string_view text, uint64_t &out_value)
{
    if (text.empty()) {
        return false;
    }
    uint64_t value = 0;
    for (char ch : text) {
        uint8_t digit = 0;
        if (ch >= '0' && ch <= '9') {
            digit = static_cast<uint8_t>(ch - '0');
        } else if (ch >= 'a' && ch <= 'f') {
            digit = static_cast<uint8_t>(ch - 'a' + 10);
        } else if (ch >= 'A' && ch <= 'F') {
            digit = static_cast<uint8_t>(ch - 'A' + 10);
        } else {
            return false;
        }
        if (value > (std::numeric_limits<uint64_t>::max() >> 4)) {
            return false;
        }
        value = (value << 4) | digit;
    }
    out_value = value;
    return true;
}

bool parse_digest_algorithm(std::string_view text, const EVP_MD *&out_md, std::string &out_name)
{
    std::string algo = trim_copy(std::string(text));
    if (algo.empty() || iequals(algo, "MD5")) {
        out_md = EVP_md5();
        out_name = "MD5";
        return true;
    }
    if (iequals(algo, "SHA-256") || iequals(algo, "SHA256")) {
        out_md = EVP_sha256();
        out_name = "SHA-256";
        return true;
    }
    return false;
}

std::string method_name_or_unknown(RtspMethod method)
{
    const char *name = method_to_string(method);
    if (!name) {
        return "UNKNOWN";
    }
    return name;
}

bool is_auth_exempt_method(RtspMethod method)
{
    return method == RtspMethod::options;
}

bool is_scale_supported(double scale)
{
    constexpr double kMinScale = 0.125;
    constexpr double kMaxScale = 16.0;
    return scale >= kMinScale && scale <= kMaxScale;
}

bool is_supported_announce_codec(const std::string &codec)
{
    return codec == "H264" || codec == "H265" || codec == "AAC" || codec == "OPUS";
}

std::string build_announce_fingerprint(const RtspSdpDescription &description)
{
    std::ostringstream oss;
    for (const auto &media : description.media) {
        oss << media.kind << ':' << media.payload_type << ':' << media.codec << ':' << media.clock_rate << ';';
    }
    return oss.str();
}

bool parse_range_header(const RtspRequest &request, std::string &out_range)
{
    const std::string *range_text = request.header("Range");
    if (!range_text) {
        return true;
    }

    if (range_text->rfind("npt=", 0) != 0) {
        return false;
    }

    const std::string value = range_text->substr(4);
    const auto dash = value.find('-');
    if (dash == std::string::npos) {
        return false;
    }

    const std::string start_text = value.substr(0, dash);
    const std::string end_text = value.substr(dash + 1);

    auto parse_npt_point = [](const std::string &text, double &out_seconds, bool &is_now) -> bool {
        if (text.empty()) {
            return false;
        }
        if (text == "now") {
            is_now = true;
            out_seconds = 0.0;
            return true;
        }
        try {
            std::size_t consumed = 0;
            const double v = std::stod(text, &consumed);
            if (consumed != text.size()) {
                return false;
            }
            if (v < 0.0) {
                return false;
            }
            is_now = false;
            out_seconds = v;
            return true;
        } catch (...) {
            return false;
        }
    };

    bool has_start = false;
    bool has_end = false;
    bool start_is_now = false;
    bool end_is_now = false;
    double start_seconds = 0.0;
    double end_seconds = 0.0;

    if (!start_text.empty()) {
        if (!parse_npt_point(start_text, start_seconds, start_is_now)) {
            return false;
        }
        has_start = true;
    }
    if (!end_text.empty()) {
        if (!parse_npt_point(end_text, end_seconds, end_is_now)) {
            return false;
        }
        has_end = true;
    }

    if (!has_start && !has_end) {
        return false;
    }
    if (end_is_now) {
        return false;
    }
    if (has_start && has_end && start_seconds > end_seconds) {
        return false;
    }

    auto format_npt = [](double seconds) -> std::string {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(3) << seconds;
        return oss.str();
    };

    std::string normalized = "npt=";
    if (has_start) {
        if (start_is_now) {
            normalized += "now";
        } else {
            normalized += format_npt(start_seconds);
        }
    }
    normalized += "-";
    if (has_end) {
        normalized += format_npt(end_seconds);
    }

    out_range = normalized;
    return true;
}

std::string build_rtp_info_for_tracks(const std::vector<std::string> &tracks)
{
    std::string out;
    for (std::size_t i = 0; i < tracks.size(); ++i) {
        if (i > 0) {
            out.append(",");
        }
        out.append("url=");
        out.append(tracks[i]);
        out.append(";seq=0;rtptime=0");
    }
    return out;
}

std::string session_header_value(const RtspRequest &request)
{
    const std::string *value = request.header("Session");
    if (!value || value->empty()) {
        return {};
    }
    const auto semicolon = value->find(';');
    if (semicolon == std::string::npos) {
        return *value;
    }
    return value->substr(0, semicolon);
}

void set_session_lookup_error(
    const std::string &sid,
    const std::map<std::string, uint64_t> &expired_sessions,
    RtspResponse &response)
{
    if (sid.empty()) {
        response.status = RtspStatusCode::session_not_found;
        return;
    }

    if (expired_sessions.find(sid) != expired_sessions.end()) {
        response.status = RtspStatusCode::request_timeout;
        response.headers["Session"] = sid;
        return;
    }

    response.status = RtspStatusCode::session_not_found;
}

uint32_t media_bridge_ssrc(const std::string &sid)
{
    const uint32_t seed = static_cast<uint32_t>(std::hash<std::string>{}(sid));
    return seed == 0 ? 1u : seed;
}

bool send_udp_payload(const std::string &target_ip, int target_port, std::string_view payload)
{
    if (target_ip.empty() || target_port <= 0 || target_port > 65535 || payload.empty()) {
        return false;
    }

    ::yuan::net::InetAddress target(target_ip, target_port);
    const int fd = target.is_ipv6() ? ::yuan::net::socket::create_ipv6_udp_socket(false)
                                    : ::yuan::net::socket::create_ipv4_udp_socket(false);
    if (fd < 0) {
        return false;
    }

    const sockaddr_storage target_addr = target.to_sockaddr();
    const socklen_t target_len = static_cast<socklen_t>(target.is_ipv6() ? sizeof(::sockaddr_in6) : sizeof(::sockaddr_in));

#ifdef _WIN32
    const int sent = ::sendto(
        fd,
        payload.data(),
        static_cast<int>(payload.size()),
        0,
        reinterpret_cast<const sockaddr *>(&target_addr),
        target_len);
#else
    const ssize_t sent = ::sendto(
        fd,
        payload.data(),
        payload.size(),
        0,
        reinterpret_cast<const sockaddr *>(&target_addr),
        target_len);
#endif

    ::yuan::net::socket::close_fd(fd);
    return sent == static_cast<decltype(sent)>(payload.size());
}

uint64_t udp_retry_backoff_ms(uint32_t retry_count, uint64_t base_ms, uint64_t max_ms)
{
    if (base_ms == 0) {
        base_ms = 1;
    }
    if (max_ms < base_ms) {
        max_ms = base_ms;
    }

    uint64_t delay = base_ms;
    for (uint32_t i = 0; i < retry_count; ++i) {
        if (delay >= max_ms / 2) {
            delay = max_ms;
            break;
        }
        delay *= 2;
    }
    return delay > max_ms ? max_ms : delay;
}

} // namespace

RtspServer::RtspServer() = default;

RtspServer::~RtspServer()
{
    stop();
    listener_.close();
}

void RtspServer::configure_basic_auth(std::string realm, std::string username, std::string password)
{
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    basic_auth_realm_ = std::move(realm);
    basic_auth_username_ = std::move(username);
    basic_auth_password_ = std::move(password);
    basic_auth_enabled_ = !basic_auth_username_.empty();
}

void RtspServer::configure_digest_auth(std::string realm, std::string username, std::string password)
{
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    digest_auth_realm_ = std::move(realm);
    digest_auth_username_ = std::move(username);
    digest_auth_password_ = std::move(password);
    digest_auth_enabled_ = !digest_auth_username_.empty();
}

void RtspServer::clear_basic_auth()
{
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    basic_auth_enabled_ = false;
    basic_auth_realm_.clear();
    basic_auth_username_.clear();
    basic_auth_password_.clear();
}

void RtspServer::clear_digest_auth()
{
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    digest_auth_enabled_ = false;
    digest_auth_realm_.clear();
    digest_auth_username_.clear();
    digest_auth_password_.clear();
    digest_nonces_.clear();
    digest_nonce_nc_watermark_.clear();
}

void RtspServer::configure_acl(const RtspAclConfig &config)
{
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    acl_config_ = config;
}

void RtspServer::clear_acl()
{
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    acl_config_ = RtspAclConfig{};
}

void RtspServer::configure_rate_limit(const RtspRateLimitConfig &config)
{
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    rate_limit_config_ = config;
}

void RtspServer::clear_rate_limit()
{
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    rate_limit_config_ = RtspRateLimitConfig{};
    client_rate_states_.clear();
}

void RtspServer::configure_observability(const RtspObservabilityConfig &config)
{
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    observability_config_ = config;
    if (observability_config_.max_audit_events == 0) {
        observability_config_.max_audit_events = 1;
    }
    while (audit_events_.size() > observability_config_.max_audit_events) {
        audit_events_.pop_front();
    }
}

std::size_t RtspServer::outbound_packet_count() const
{
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    return outbound_packets_.size();
}

std::size_t RtspServer::flush_udp_outbound_packets(const std::string &client_ip, std::size_t max_packets)
{
    if (client_ip.empty() || max_packets == 0) {
        return 0;
    }

    std::size_t flushed = 0;
    while (flushed < max_packets) {
        RtspOutboundPacket pending;
        bool should_retry = false;
        {
            std::lock_guard<std::mutex> lock(sessions_mutex_);
            auto it = outbound_packets_.begin();
            for (; it != outbound_packets_.end(); ++it) {
                if (it->transport == RtspOutboundTransport::udp_unicast &&
                    (it->next_retry_after_ms == 0 || it->next_retry_after_ms <= now_ms())) {
                    pending = std::move(*it);
                    outbound_packets_.erase(it);
                    break;
                }
            }
        }

        if (pending.bytes.empty()) {
            break;
        }

        const bool ok = send_udp_payload(client_ip, pending.udp_remote_port, std::string_view(pending.bytes));
        {
            std::lock_guard<std::mutex> lock(sessions_mutex_);
            if (ok) {
                ++metrics_.outbound_udp_sent;
            } else {
                ++metrics_.outbound_udp_failed;
                ++metrics_.outbound_udp_failed_by_track[pending.track_uri.empty() ? std::string("-") : pending.track_uri];
                should_retry = pending.retry_count < pending.max_retries;
                RtspAuditEvent event;
                event.timestamp_ms = now_ms();
                event.client_ip = client_ip;
                event.session_id = pending.session_id;
                event.method = "OUTBOUND";
                event.status_code = 0;
                event.action = should_retry ? "udp_retry" : "udp_drop";
                event.result = should_retry ? "scheduled" : "dropped";
                event.detail = "track=" + pending.track_uri + ",port=" + std::to_string(pending.udp_remote_port);
                record_audit_event_locked(std::move(event));
            }
        }

        if (!ok && should_retry) {
            pending.retry_count += 1;
            pending.next_retry_after_ms = now_ms() + udp_retry_backoff_ms(
                pending.retry_count,
                observability_config_.udp_retry_base_backoff_ms,
                observability_config_.udp_retry_max_backoff_ms);
            std::lock_guard<std::mutex> lock(sessions_mutex_);
            ++metrics_.outbound_udp_retried;
            outbound_packets_.push_back(std::move(pending));
        } else if (!ok) {
            std::lock_guard<std::mutex> lock(sessions_mutex_);
            ++metrics_.outbound_udp_dropped;
        }
        ++flushed;
    }

    return flushed;
}

std::vector<RtspOutboundPacket> RtspServer::drain_outbound_packets(std::size_t max_packets)
{
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    std::vector<RtspOutboundPacket> out;
    if (max_packets == 0 || outbound_packets_.empty()) {
        return out;
    }

    const std::size_t take = std::min(max_packets, outbound_packets_.size());
    out.reserve(take);
    for (std::size_t i = 0; i < take; ++i) {
        out.push_back(std::move(outbound_packets_.front()));
        outbound_packets_.pop_front();
    }
    return out;
}

bool RtspServer::inject_rtp_packet(
    const std::string &session_id,
    const ::yuan::net::rtc::RtcPacket &packet,
    uint64_t arrival_time_ms)
{
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    auto session_it = sessions_.find(session_id);
    if (session_it == sessions_.end()) {
        return false;
    }

    auto bridge_it = media_bridges_.find(session_id);
    if (bridge_it == media_bridges_.end()) {
        bridge_it = media_bridges_.emplace(session_id, SessionMediaBridge{}).first;
        bridge_it->second.rtcp_session.set_local_ssrc(media_bridge_ssrc(session_id));
    }

    const bool ok = bridge_it->second.rtp_manager.on_packet_received(packet, arrival_time_ms);
    if (ok) {
        session_it->second.touch(arrival_time_ms);
    }
    return ok;
}

bool RtspServer::build_receiver_report(const std::string &session_id, ::yuan::net::rtcp::RtcpPacket &out_packet)
{
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    auto session_it = sessions_.find(session_id);
    if (session_it == sessions_.end()) {
        return false;
    }

    auto bridge_it = media_bridges_.find(session_id);
    if (bridge_it == media_bridges_.end()) {
        return false;
    }

    out_packet = bridge_it->second.rtcp_session.build_receiver_report(bridge_it->second.rtp_manager);
    return true;
}

bool RtspServer::build_sender_report(const std::string &session_id, ::yuan::net::rtcp::RtcpPacket &out_packet)
{
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    auto session_it = sessions_.find(session_id);
    if (session_it == sessions_.end()) {
        return false;
    }

    auto bridge_it = media_bridges_.find(session_id);
    if (bridge_it == media_bridges_.end()) {
        return false;
    }

    out_packet = bridge_it->second.rtcp_session.build_sender_report(bridge_it->second.rtp_manager);
    return true;
}

bool RtspServer::build_interleaved_rtp_frame(
    const std::string &session_id,
    const std::string &track_uri,
    const ::yuan::net::rtc::RtcPacket &packet,
    RtspInterleavedFrame &out_frame)
{
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    auto session_it = sessions_.find(session_id);
    if (session_it == sessions_.end()) {
        return false;
    }

    uint8_t rtp_channel = 0;
    uint8_t rtcp_channel = 0;
    if (!session_it->second.resolve_track_interleaved_channels(track_uri, rtp_channel, rtcp_channel)) {
        (void)rtcp_channel;
        return false;
    }

    ::yuan::buffer::ByteBuffer payload;
    if (!packet.serialize(payload)) {
        return false;
    }

    if (payload.readable_bytes() > 0xFFFF) {
        return false;
    }

    std::string frame;
    frame.reserve(4 + payload.readable_bytes());
    frame.push_back('$');
    frame.push_back(static_cast<char>(rtp_channel));
    const uint16_t size = static_cast<uint16_t>(payload.readable_bytes());
    frame.push_back(static_cast<char>((size >> 8) & 0xFF));
    frame.push_back(static_cast<char>(size & 0xFF));
    frame.append(payload.read_ptr(), payload.readable_bytes());

    out_frame.channel = rtp_channel;
    out_frame.bytes = std::move(frame);
    return true;
}

bool RtspServer::build_interleaved_receiver_report_frame(
    const std::string &session_id,
    const std::string &track_uri,
    RtspInterleavedFrame &out_frame)
{
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    auto session_it = sessions_.find(session_id);
    if (session_it == sessions_.end()) {
        return false;
    }

    uint8_t rtp_channel = 0;
    uint8_t rtcp_channel = 0;
    if (!session_it->second.resolve_track_interleaved_channels(track_uri, rtp_channel, rtcp_channel)) {
        (void)rtp_channel;
        return false;
    }

    auto bridge_it = media_bridges_.find(session_id);
    if (bridge_it == media_bridges_.end()) {
        return false;
    }

    ::yuan::net::rtcp::RtcpPacket rr =
        bridge_it->second.rtcp_session.build_receiver_report(bridge_it->second.rtp_manager);

    ::yuan::buffer::ByteBuffer payload;
    if (!rr.serialize(payload)) {
        return false;
    }
    if (payload.readable_bytes() > 0xFFFF) {
        return false;
    }

    std::string frame;
    frame.reserve(4 + payload.readable_bytes());
    frame.push_back('$');
    frame.push_back(static_cast<char>(rtcp_channel));
    const uint16_t size = static_cast<uint16_t>(payload.readable_bytes());
    frame.push_back(static_cast<char>((size >> 8) & 0xFF));
    frame.push_back(static_cast<char>(size & 0xFF));
    frame.append(payload.read_ptr(), payload.readable_bytes());

    out_frame.channel = rtcp_channel;
    out_frame.bytes = std::move(frame);
    return true;
}

bool RtspServer::build_interleaved_sender_report_frame(
    const std::string &session_id,
    const std::string &track_uri,
    RtspInterleavedFrame &out_frame)
{
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    auto session_it = sessions_.find(session_id);
    if (session_it == sessions_.end()) {
        return false;
    }

    uint8_t rtp_channel = 0;
    uint8_t rtcp_channel = 0;
    if (!session_it->second.resolve_track_interleaved_channels(track_uri, rtp_channel, rtcp_channel)) {
        (void)rtp_channel;
        return false;
    }

    auto bridge_it = media_bridges_.find(session_id);
    if (bridge_it == media_bridges_.end()) {
        return false;
    }

    ::yuan::net::rtcp::RtcpPacket sr =
        bridge_it->second.rtcp_session.build_sender_report(bridge_it->second.rtp_manager);

    ::yuan::buffer::ByteBuffer payload;
    if (!sr.serialize(payload)) {
        return false;
    }
    if (payload.readable_bytes() > 0xFFFF) {
        return false;
    }

    std::string frame;
    frame.reserve(4 + payload.readable_bytes());
    frame.push_back('$');
    frame.push_back(static_cast<char>(rtcp_channel));
    const uint16_t size = static_cast<uint16_t>(payload.readable_bytes());
    frame.push_back(static_cast<char>((size >> 8) & 0xFF));
    frame.push_back(static_cast<char>(size & 0xFF));
    frame.append(payload.read_ptr(), payload.readable_bytes());

    out_frame.channel = rtcp_channel;
    out_frame.bytes = std::move(frame);
    return true;
}

bool RtspServer::queue_interleaved_report_locked(
    const std::string &session_id,
    const std::string &track_uri,
    bool sender_report)
{
    auto session_it = sessions_.find(session_id);
    if (session_it == sessions_.end()) {
        return false;
    }

    uint8_t rtp_channel = 0;
    uint8_t rtcp_channel = 0;
    if (!session_it->second.resolve_track_interleaved_channels(track_uri, rtp_channel, rtcp_channel)) {
        return false;
    }

    auto bridge_it = media_bridges_.find(session_id);
    if (bridge_it == media_bridges_.end()) {
        return false;
    }

    ::yuan::net::rtcp::RtcpPacket packet = sender_report
                                               ? bridge_it->second.rtcp_session.build_sender_report(bridge_it->second.rtp_manager)
                                               : bridge_it->second.rtcp_session.build_receiver_report(bridge_it->second.rtp_manager);
    ::yuan::buffer::ByteBuffer payload;
    if (!packet.serialize(payload) || payload.readable_bytes() > 0xFFFF) {
        return false;
    }

    std::string frame;
    frame.reserve(4 + payload.readable_bytes());
    frame.push_back('$');
    frame.push_back(static_cast<char>(rtcp_channel));
    const uint16_t size = static_cast<uint16_t>(payload.readable_bytes());
    frame.push_back(static_cast<char>((size >> 8) & 0xFF));
    frame.push_back(static_cast<char>(size & 0xFF));
    frame.append(payload.read_ptr(), payload.readable_bytes());

    RtspOutboundPacket out;
    out.transport = RtspOutboundTransport::interleaved_tcp;
    out.session_id = session_id;
    out.track_uri = track_uri;
    out.is_rtcp = true;
    out.interleaved_channel = rtcp_channel;
    out.bytes = std::move(frame);
    outbound_packets_.push_back(std::move(out));
    return true;
}

bool RtspServer::queue_udp_report_locked(
    const std::string &session_id,
    const std::string &track_uri,
    bool sender_report)
{
    auto session_it = sessions_.find(session_id);
    if (session_it == sessions_.end()) {
        return false;
    }

    RtspTransportSpec transport;
    if (!session_it->second.resolve_track_transport(track_uri, transport) ||
        transport.transport != RtspLowerTransport::rtp_avp_udp) {
        return false;
    }

    int remote_rtcp = transport.client_rtcp_port;
    if (remote_rtcp < 0 && transport.client_rtp_port >= 0) {
        remote_rtcp = transport.client_rtp_port + 1;
    }
    if (remote_rtcp <= 0 || remote_rtcp > 65535) {
        return false;
    }

    auto bridge_it = media_bridges_.find(session_id);
    if (bridge_it == media_bridges_.end()) {
        return false;
    }

    ::yuan::net::rtcp::RtcpPacket packet = sender_report
                                               ? bridge_it->second.rtcp_session.build_sender_report(bridge_it->second.rtp_manager)
                                               : bridge_it->second.rtcp_session.build_receiver_report(bridge_it->second.rtp_manager);
    ::yuan::buffer::ByteBuffer payload;
    if (!packet.serialize(payload) || payload.readable_bytes() == 0) {
        return false;
    }

    RtspOutboundPacket out;
    out.transport = RtspOutboundTransport::udp_unicast;
    out.session_id = session_id;
    out.track_uri = track_uri;
    out.is_rtcp = true;
    out.udp_remote_port = remote_rtcp;
    out.max_retries = observability_config_.udp_retry_max_retries;
    out.bytes.assign(payload.read_ptr(), payload.readable_bytes());
    outbound_packets_.push_back(std::move(out));
    return true;
}

bool RtspServer::maybe_queue_auto_feedback_locked(
    const std::string &session_id,
    const std::string &track_uri,
    bool frame_is_rtcp,
    uint64_t now_ms)
{
    auto session_it = sessions_.find(session_id);
    if (session_it == sessions_.end()) {
        return false;
    }

    RtspTransportSpec transport;
    if (!session_it->second.resolve_track_transport(track_uri, transport)) {
        return false;
    }

    const std::string key = session_id + "|" + track_uri;
    constexpr uint64_t kRrIntervalMs = 1000;
    constexpr uint64_t kSrIntervalMs = 5000;

    if (!frame_is_rtcp) {
        uint64_t &last_rr = feedback_rr_last_ms_[key];
        if (last_rr != 0 && now_ms >= last_rr && (now_ms - last_rr) < kRrIntervalMs) {
            return false;
        }
        last_rr = now_ms;
        if (transport.transport == RtspLowerTransport::rtp_avp_tcp) {
            return queue_interleaved_report_locked(session_id, track_uri, false);
        }
        if (transport.transport == RtspLowerTransport::rtp_avp_udp) {
            return queue_udp_report_locked(session_id, track_uri, false);
        }
        return false;
    }

    if (session_it->second.state() != RtspSessionState::recording) {
        return false;
    }
    uint64_t &last_sr = feedback_sr_last_ms_[key];
    if (last_sr != 0 && now_ms >= last_sr && (now_ms - last_sr) < kSrIntervalMs) {
        return false;
    }
    last_sr = now_ms;
    if (transport.transport == RtspLowerTransport::rtp_avp_tcp) {
        return queue_interleaved_report_locked(session_id, track_uri, true);
    }
    if (transport.transport == RtspLowerTransport::rtp_avp_udp) {
        return queue_udp_report_locked(session_id, track_uri, true);
    }
    return false;
}

bool RtspServer::media_bridge_snapshot(const std::string &session_id, RtspMediaBridgeSnapshot &out_snapshot) const
{
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    auto session_it = sessions_.find(session_id);
    if (session_it == sessions_.end()) {
        return false;
    }

    auto bridge_it = media_bridges_.find(session_id);
    if (bridge_it == media_bridges_.end()) {
        return false;
    }

    out_snapshot.rtp_session_count = bridge_it->second.rtp_manager.session_count();
    out_snapshot.rejected_rtp_packets = bridge_it->second.rtp_manager.rejected_packets();

    const auto rtcp_stats = bridge_it->second.rtcp_session.stats_snapshot();
    out_snapshot.rtcp.has_sender_activity = rtcp_stats.has_sender_activity;
    out_snapshot.rtcp.sender_ntp_timestamp = rtcp_stats.sender_ntp_timestamp;
    out_snapshot.rtcp.sender_rtp_timestamp = rtcp_stats.sender_rtp_timestamp;
    out_snapshot.rtcp.sender_packet_count = rtcp_stats.sender_packet_count;
    out_snapshot.rtcp.sender_octet_count = rtcp_stats.sender_octet_count;
    out_snapshot.rtcp.last_sr_lsr = rtcp_stats.last_sr_lsr;
    out_snapshot.rtcp.last_sr_delay_65536 = rtcp_stats.last_sr_delay_65536;
    out_snapshot.rtcp.rr_reports_built = rtcp_stats.rr_reports_built;
    out_snapshot.rtcp.sr_reports_built = rtcp_stats.sr_reports_built;
    return true;
}

RtspMetricsSnapshot RtspServer::metrics_snapshot() const
{
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    return metrics_;
}

std::vector<RtspAuditEvent> RtspServer::recent_audit_events(std::size_t max_events) const
{
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    std::vector<RtspAuditEvent> out;
    if (max_events == 0 || audit_events_.empty()) {
        return out;
    }

    const std::size_t take = std::min(max_events, audit_events_.size());
    out.reserve(take);
    const auto begin = audit_events_.end() - static_cast<std::ptrdiff_t>(take);
    out.insert(out.end(), begin, audit_events_.end());
    return out;
}

void RtspServer::record_audit_event_locked(RtspAuditEvent event)
{
    if (!observability_config_.enable_audit) {
        return;
    }

    audit_events_.push_back(std::move(event));
    while (audit_events_.size() > observability_config_.max_audit_events) {
        audit_events_.pop_front();
    }
}

InterleavedStatsSnapshot RtspServer::interleaved_stats_snapshot() const
{
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    return interleaved_stats_;
}

SecurityStatsSnapshot RtspServer::security_stats_snapshot() const
{
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    return security_stats_;
}

RtspServer::InterleavedFrameResult RtspServer::handle_interleaved_frame(
    uint8_t channel,
    std::string_view payload,
    uint64_t arrival_time_ms)
{
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    const std::size_t expired_before = expired_sessions_.size();
    collect_expired_sessions_locked(arrival_time_ms);
    const bool had_new_expired = expired_sessions_.size() > expired_before;

    std::string matched_sid;
    std::string matched_track;
    bool is_rtcp = false;
    for (auto &entry : sessions_) {
        bool channel_is_rtcp = false;
        std::string track_uri;
        if (entry.second.resolve_track_by_interleaved_channel(channel, track_uri, channel_is_rtcp)) {
            matched_sid = entry.first;
            matched_track = std::move(track_uri);
            is_rtcp = channel_is_rtcp;
            break;
        }
    }

    if (matched_sid.empty()) {
        if (had_new_expired) {
            ++interleaved_stats_.session_expired;
            return InterleavedFrameResult::session_expired;
        }
        ++interleaved_stats_.unknown_channel;
        return InterleavedFrameResult::unknown_channel;
    }

    auto bridge_iter = media_bridges_.find(matched_sid);
    if (bridge_iter == media_bridges_.end()) {
        bridge_iter = media_bridges_.emplace(matched_sid, SessionMediaBridge{}).first;
        bridge_iter->second.rtcp_session.set_local_ssrc(media_bridge_ssrc(matched_sid));
    }

    auto session_iter = sessions_.find(matched_sid);
    if (session_iter == sessions_.end()) {
        ++interleaved_stats_.unknown_channel;
        return InterleavedFrameResult::unknown_channel;
    }

    ::yuan::buffer::ByteBuffer media_buf;
    media_buf.append(payload.data(), payload.size());

    if (!is_rtcp) {
        ::yuan::net::rtc::RtcPacket packet;
        if (!packet.deserialize(media_buf)) {
            ++interleaved_stats_.malformed_rtp;
            return InterleavedFrameResult::malformed_rtp;
        }
        if (!bridge_iter->second.rtp_manager.on_packet_received(packet, arrival_time_ms)) {
            ++interleaved_stats_.malformed_rtp;
            return InterleavedFrameResult::malformed_rtp;
        }
        session_iter->second.touch(arrival_time_ms);
        if (!matched_track.empty()) {
            (void)maybe_queue_auto_feedback_locked(matched_sid, matched_track, false, arrival_time_ms);
        }
        ++interleaved_stats_.handled_rtp;
        return InterleavedFrameResult::handled_rtp;
    }

    ::yuan::net::rtcp::RtcpPacket packet;
    if (!packet.deserialize(media_buf)) {
        ++interleaved_stats_.malformed_rtcp;
        return InterleavedFrameResult::malformed_rtcp;
    }
    if (packet.kind == ::yuan::net::rtcp::RtcpPacket::Kind::sender_report) {
        bridge_iter->second.rtcp_session.on_sender_activity(
            packet.sender_report.ntp_timestamp,
            packet.sender_report.rtp_timestamp,
            packet.sender_report.packet_count,
            packet.sender_report.octet_count,
            arrival_time_ms);
    }
    session_iter->second.touch(arrival_time_ms);
    if (!matched_track.empty()) {
        (void)maybe_queue_auto_feedback_locked(matched_sid, matched_track, true, arrival_time_ms);
    }
    ++interleaved_stats_.handled_rtcp;
    return InterleavedFrameResult::handled_rtcp;
}

bool RtspServer::init(int port)
{
    owned_runtime_ = std::make_unique<NetworkRuntime>();
    return init(port, *owned_runtime_);
}

bool RtspServer::init(int port, NetworkRuntime &runtime)
{
    if (port <= 0 || port > 65535) {
        return false;
    }
    port_ = port;
    return listener_.bind(static_cast<uint16_t>(port), runtime);
}

void RtspServer::serve()
{
    listener_.set_connection_handler([this](AsyncConnectionContext ctx) -> coroutine::Task<void> {
        co_await handle_connection(std::move(ctx));
    });

    auto task = listener_.run_async();
    task.resume();
    task.detach();

    if (owned_runtime_) {
        owned_runtime_->run();
    }
}

void RtspServer::stop()
{
    if (owned_runtime_) {
        auto *runtime = owned_runtime_.get();
        runtime->dispatch([this, runtime]() {
            listener_.close();
            runtime->stop();
        });
        return;
    }
    listener_.close();
}

void RtspServer::set_handler(RequestHandler handler)
{
    handler_ = std::move(handler);
}

std::size_t RtspServer::session_count() const
{
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    return sessions_.size();
}

void RtspServer::collect_expired_sessions_locked(uint64_t current_ms)
{
    std::vector<std::string> expired;
    for (const auto &entry : sessions_) {
        if (entry.second.is_expired(current_ms, entry.second.timeout_ms())) {
            expired.push_back(entry.first);
        }
    }
    for (const auto &sid : expired) {
        erase_prefixed_keys(feedback_rr_last_ms_, sid + "|");
        erase_prefixed_keys(feedback_sr_last_ms_, sid + "|");
        expired_sessions_[sid] = current_ms;
        session_connection_owner_.erase(sid);
        media_bridges_.erase(sid);
        sessions_.erase(sid);
    }

    constexpr std::size_t kExpiredRecordLimit = 1024;
    while (expired_sessions_.size() > kExpiredRecordLimit) {
        expired_sessions_.erase(expired_sessions_.begin());
    }
}

RtspResponse RtspServer::handle_request(const RtspRequest &request)
{
    return handle_request_with_context(request, std::string{}, now_ms(), 0);
}

RtspResponse RtspServer::handle_request_with_context(
    const RtspRequest &request,
    const std::string &client_ip,
    uint64_t request_time_ms,
    uint64_t connection_id)
{
    RtspResponse response;
    response.cseq = request.cseq;

    if (handler_) {
        handler_(request, response);
        return response;
    }

    response.headers["Public"] = "OPTIONS, DESCRIBE, SETUP, PLAY, PAUSE, TEARDOWN, GET_PARAMETER, SET_PARAMETER, RECORD";

    const uint64_t current_ms = request_time_ms;

    std::lock_guard<std::mutex> lock(sessions_mutex_);

    const std::string client_key = client_ip.empty() ? std::string("__local__") : client_ip;
    auto &rate_state = client_rate_states_[client_key];

    ++metrics_.requests_total;
    const std::string request_method_name = method_name_or_unknown(request.method);
    ++metrics_.method_counts[request_method_name];

    RtspAuditEvent audit;
    audit.timestamp_ms = current_ms;
    audit.client_ip = client_ip;
    audit.method = request_method_name;
    audit.status_code = 0;
    audit.action = request_method_name;
    audit.result = "in_progress";

    auto finalize_and_return = [&](RtspStatusCode status, const char *result_text) -> RtspResponse {
        response.status = status;
        const uint16_t code = static_cast<uint16_t>(status);
        ++metrics_.status_counts[code];
        if (code >= 500) {
            ++metrics_.responses_5xx;
        } else if (code >= 400) {
            ++metrics_.responses_4xx;
        } else if (code >= 200 && code < 300) {
            ++metrics_.responses_2xx;
        }

        const std::string sid = session_header_value(request);
        audit.session_id = sid;
        audit.status_code = code;
        audit.result = result_text ? result_text : "unknown";
        record_audit_event_locked(audit);

        if (observability_config_.enable_log) {
            LOG_INFO(
                "[RTSP] method={} status={} client={} session={} result={} cseq={} uri={}",
                audit.method,
                audit.status_code,
                audit.client_ip.empty() ? std::string("-") : audit.client_ip,
                audit.session_id.empty() ? std::string("-") : audit.session_id,
                audit.result,
                request.cseq,
                request.uri);
        }
        return response;
    };

    if (rate_limit_config_.enabled && rate_limit_config_.window_ms > 0 && rate_limit_config_.max_requests > 0) {
        if (rate_state.window_start_ms == 0 || current_ms < rate_state.window_start_ms ||
            (current_ms - rate_state.window_start_ms) >= rate_limit_config_.window_ms) {
            rate_state.window_start_ms = current_ms;
            rate_state.request_count = 0;
        }

        if (rate_state.ban_until_ms > current_ms) {
            ++security_stats_.auth_banned;
            return finalize_and_return(RtspStatusCode::too_many_requests, "auth_banned");
        }

        ++rate_state.request_count;
        if (rate_state.request_count > rate_limit_config_.max_requests) {
            ++security_stats_.rate_limited;
            return finalize_and_return(RtspStatusCode::too_many_requests, "rate_limited");
        }
    }

    if (acl_config_.enabled) {
        bool decision = acl_config_.default_allow;
        bool has_ip = false;
        uint32_t client_ip_v4 = 0;
        if (!client_ip.empty()) {
            has_ip = parse_ipv4_address(client_ip, client_ip_v4);
        }

        if (has_ip) {
            for (const std::string &item : acl_config_.allow_ips) {
                uint32_t v = 0;
                if (parse_ipv4_address(item, v) && v == client_ip_v4) {
                    decision = true;
                    break;
                }
            }
            for (const std::string &item : acl_config_.allow_cidrs) {
                uint32_t network = 0;
                uint8_t prefix = 0;
                if (parse_cidr_v4(item, network, prefix) && ip_in_cidr_v4(client_ip_v4, network, prefix)) {
                    decision = true;
                    break;
                }
            }
            for (const std::string &item : acl_config_.deny_ips) {
                uint32_t v = 0;
                if (parse_ipv4_address(item, v) && v == client_ip_v4) {
                    decision = false;
                    break;
                }
            }
            for (const std::string &item : acl_config_.deny_cidrs) {
                uint32_t network = 0;
                uint8_t prefix = 0;
                if (parse_cidr_v4(item, network, prefix) && ip_in_cidr_v4(client_ip_v4, network, prefix)) {
                    decision = false;
                    break;
                }
            }
        }

        if (has_any_prefix_match(acl_config_.allow_uri_prefixes, request.uri)) {
            decision = true;
        }
        if (has_any_prefix_match(acl_config_.deny_uri_prefixes, request.uri)) {
            decision = false;
        }

        if (!decision) {
            ++security_stats_.acl_denied;
            return finalize_and_return(RtspStatusCode::forbidden, "acl_denied");
        }
    }

    if (!is_auth_exempt_method(request.method) && (basic_auth_enabled_ || digest_auth_enabled_)) {
        const std::string *auth = request.header("Authorization");
        bool auth_ok = false;
        bool basic_attempt = false;
        bool digest_attempt = false;
        bool digest_stale = false;

        if (auth && basic_auth_enabled_ && istarts_with(*auth, "Basic ")) {
            basic_attempt = true;
            std::string username;
            std::string password;
            auth_ok = parse_basic_credentials(*auth, username, password) &&
                      username == basic_auth_username_ && password == basic_auth_password_;
            if (!auth_ok) {
                ++security_stats_.auth_basic_fail;
            }
        } else if (auth && digest_auth_enabled_ && istarts_with(*auth, "Digest ")) {
            digest_attempt = true;
            DigestAuthParams params;
            if (parse_digest_credentials(*auth, params)) {
                const std::string realm = digest_auth_realm_.empty() ? std::string("rtsp") : digest_auth_realm_;
                auto nonce_iter = digest_nonces_.find(params.nonce);
                const bool nonce_known = nonce_iter != digest_nonces_.end();
                const bool nonce_not_expired = nonce_known && nonce_iter->second.expire_ms >= current_ms;
                if (!nonce_not_expired) {
                    digest_stale = true;
                }
                const bool user_ok = params.username == digest_auth_username_;
                const bool realm_ok = params.realm == realm;
                const bool uri_ok = params.uri == request.uri;
                const bool qop_ok = is_digest_qop_auth(params.qop) &&
                                    (params.qop.empty() || (!params.nc.empty() && !params.cnonce.empty()));
                const bool opaque_ok = !nonce_known || nonce_iter->second.opaque.empty() || params.opaque == nonce_iter->second.opaque;

                const EVP_MD *digest_md = nullptr;
                std::string digest_algo;
                const bool algo_ok = parse_digest_algorithm(params.algorithm, digest_md, digest_algo);

                bool nonce_count_ok = true;
                if (!params.qop.empty()) {
                    uint64_t nc_value = 0;
                    if (!parse_hex_u64(params.nc, nc_value)) {
                        nonce_count_ok = false;
                    } else {
                        const std::string watermark_key = params.nonce + "|" + params.username + "|" + params.cnonce;
                        const auto wm_it = digest_nonce_nc_watermark_.find(watermark_key);
                        if (wm_it != digest_nonce_nc_watermark_.end() && nc_value <= wm_it->second) {
                            nonce_count_ok = false;
                        }
                    }
                }

                if (nonce_not_expired && user_ok && realm_ok && uri_ok && qop_ok && opaque_ok && algo_ok && nonce_count_ok) {
                    const std::string ha1 = digest_hex(params.username + ":" + realm + ":" + digest_auth_password_, digest_md);
                    const std::string ha2 = digest_hex(std::string(method_to_string(request.method)) + ":" + params.uri, digest_md);
                    std::string expected;
                    if (params.qop.empty()) {
                        expected = digest_hex(ha1 + ":" + params.nonce + ":" + ha2, digest_md);
                    } else {
                        expected = digest_hex(ha1 + ":" + params.nonce + ":" + params.nc + ":" + params.cnonce + ":" + params.qop + ":" + ha2, digest_md);
                    }

                    std::string provided = params.response;
                    std::transform(provided.begin(), provided.end(), provided.begin(), ascii_tolower);
                    auth_ok = !expected.empty() && equals_constant_time(provided, expected);
                    if (auth_ok && !params.qop.empty()) {
                        uint64_t nc_value = 0;
                        if (parse_hex_u64(params.nc, nc_value)) {
                            const std::string watermark_key = params.nonce + "|" + params.username + "|" + params.cnonce;
                            digest_nonce_nc_watermark_[watermark_key] = nc_value;
                        }
                    }
                }
            }

            if (!auth_ok) {
                ++security_stats_.auth_digest_fail;
            }
        }

        if (!auth_ok) {
            ++rate_state.auth_failures;
            if (rate_limit_config_.enabled && rate_limit_config_.auth_fail_limit > 0 &&
                rate_state.auth_failures >= rate_limit_config_.auth_fail_limit) {
                if (rate_limit_config_.auth_ban_ms > 0) {
                    rate_state.ban_until_ms = current_ms + rate_limit_config_.auth_ban_ms;
                }
                rate_state.auth_failures = 0;
            }

            response.status = RtspStatusCode::unauthorized;
            std::vector<std::string> challenges;
            challenges.reserve(2);

            if (basic_auth_enabled_) {
                const std::string realm = basic_auth_realm_.empty() ? std::string("rtsp") : basic_auth_realm_;
                challenges.push_back("Basic realm=\"" + escape_header_quoted(realm) + "\"");
            }

            if (digest_auth_enabled_) {
                const std::string realm = digest_auth_realm_.empty() ? std::string("rtsp") : digest_auth_realm_;
                const std::string nonce = generate_auth_nonce(current_ms);
                const std::string opaque = md5_hex(nonce + ":" + realm + ":opaque");
                digest_nonces_[nonce] = NonceEntry{current_ms, current_ms + 120000, opaque};
                challenges.push_back(
                    "Digest realm=\"" + escape_header_quoted(realm) +
                    "\", nonce=\"" + nonce +
                    "\", opaque=\"" + opaque +
                    "\", algorithm=MD5, qop=\"auth\"" +
                    (digest_stale ? ", stale=TRUE" : ""));
                challenges.push_back(
                    "Digest realm=\"" + escape_header_quoted(realm) +
                    "\", nonce=\"" + nonce +
                    "\", opaque=\"" + opaque +
                    "\", algorithm=SHA-256, qop=\"auth\"" +
                    (digest_stale ? ", stale=TRUE" : ""));
            }

            if (!challenges.empty()) {
                response.headers["WWW-Authenticate"] = join_challenges(challenges);
            }

            if (!basic_attempt && !digest_attempt && rate_limit_config_.enabled && rate_limit_config_.auth_ban_ms > 0 &&
                rate_limit_config_.auth_fail_limit == 0) {
                rate_state.ban_until_ms = current_ms + rate_limit_config_.auth_ban_ms;
            }
            return finalize_and_return(response.status, "auth_failed");
        }

        ++security_stats_.auth_success;
        rate_state.auth_failures = 0;
    }

    collect_expired_sessions_locked(current_ms);

    if (request.method == RtspMethod::options) {
        response.status = RtspStatusCode::ok;
        return finalize_and_return(response.status, "ok_options");
    }

    if (request.method == RtspMethod::describe) {
        RtspSdpDescription desc;
        desc.session_name = "rtsp-default";
        desc.media.push_back({"video", 96, "H264", 90000});
        if (serialize_sdp(desc, response.body)) {
            response.status = RtspStatusCode::ok;
            response.headers["Content-Type"] = "application/sdp";
        } else {
            response.status = RtspStatusCode::internal_server_error;
        }
            return finalize_and_return(response.status, response.status == RtspStatusCode::ok ? "ok_describe" : "describe_error");
    }

    if (request.method == RtspMethod::announce) {
        const std::string sid = session_header_value(request);
        if (sid.empty()) {
            response.status = RtspStatusCode::session_not_found;
            return finalize_and_return(response.status, "announce_missing_session");
        }

        auto iter = sessions_.find(sid);
        if (iter == sessions_.end()) {
            set_session_lookup_error(sid, expired_sessions_, response);
            return finalize_and_return(response.status, response.status == RtspStatusCode::request_timeout ? "announce_session_expired" : "announce_session_not_found");
        }

        RtspSession &session = iter->second;
        if (!session.validate_cseq(request.cseq)) {
            response.status = RtspStatusCode::bad_request;
            return finalize_and_return(response.status, "announce_bad_cseq");
        }

        RtspSdpDescription announced;
        if (!parse_sdp(request.body, announced) || announced.media.empty()) {
            response.status = RtspStatusCode::bad_request;
            return finalize_and_return(response.status, "announce_bad_sdp");
        }
        for (const auto &media : announced.media) {
            if (media.codec.empty() || !is_supported_announce_codec(media.codec)) {
                response.status = RtspStatusCode::parameter_not_understood;
                return finalize_and_return(response.status, "announce_unsupported_codec");
            }
        }

        const std::string fingerprint = build_announce_fingerprint(announced);
        if (!session.on_announce(announced.media.size(), fingerprint)) {
            response.status = RtspStatusCode::method_not_valid_in_this_state;
            return finalize_and_return(response.status, "announce_invalid_state");
        }
        session.touch(current_ms);
        response.status = RtspStatusCode::ok;
        response.headers["Session"] = session.id() + ";timeout=" + std::to_string(session.timeout_ms() / 1000);
        return finalize_and_return(response.status, "ok_announce");
    }

    if (request.method == RtspMethod::setup) {
        const std::string *transport_text = request.header("Transport");
        if (!transport_text) {
            response.status = RtspStatusCode::unsupported_transport;
            return finalize_and_return(response.status, "setup_missing_transport");
        }

        std::vector<RtspTransportSpec> candidates;
        if (!parse_transport_candidates(*transport_text, candidates) || candidates.empty()) {
            response.status = RtspStatusCode::unsupported_transport;
            return finalize_and_return(response.status, "setup_bad_transport");
        }

        RtspTransportSpec spec;
        bool found_candidate = false;
        bool saw_parameter_error = false;
        for (const RtspTransportSpec &candidate : candidates) {
            if (!supports_transport_candidate(candidate)) {
                continue;
            }
            if (!validate_client_ports(candidate) || !validate_interleaved_channels(candidate)) {
                saw_parameter_error = true;
                continue;
            }
            spec = candidate;
            found_candidate = true;
            break;
        }
        if (!found_candidate) {
            response.status = saw_parameter_error ? RtspStatusCode::parameter_not_understood
                                                  : RtspStatusCode::unsupported_transport;
            return finalize_and_return(response.status, response.status == RtspStatusCode::parameter_not_understood ? "setup_transport_param_error" : "setup_transport_unsupported");
        }

        std::string sid = session_header_value(request);
        if (sid.empty()) {
            sid = generate_session_id();
        }

        auto iter = sessions_.find(sid);
        if (iter == sessions_.end()) {
            iter = sessions_.emplace(sid, RtspSession(sid)).first;
            expired_sessions_.erase(sid);
            media_bridges_[sid] = SessionMediaBridge{};
            media_bridges_[sid].rtcp_session.set_local_ssrc(media_bridge_ssrc(sid));
        } else if (media_bridges_.find(sid) == media_bridges_.end()) {
            media_bridges_[sid] = SessionMediaBridge{};
            media_bridges_[sid].rtcp_session.set_local_ssrc(media_bridge_ssrc(sid));
        }

        if (connection_id != 0) {
            auto owner_it = session_connection_owner_.find(sid);
            if (owner_it == session_connection_owner_.end() || owner_it->second == connection_id) {
                session_connection_owner_[sid] = connection_id;
            }
        }

        RtspSession &session = iter->second;
        if (session.has_interleaved_channel_conflict(spec)) {
            response.status = RtspStatusCode::unsupported_transport;
            return finalize_and_return(response.status, "setup_channel_conflict");
        }

        if (!session.validate_cseq(request.cseq) || !session.on_setup(request.uri, spec)) {
            response.status = RtspStatusCode::method_not_valid_in_this_state;
            return finalize_and_return(response.status, "setup_invalid_state");
        }

        const uint64_t timeout_ms = parse_timeout_ms_from_session_header(request);
        session.set_timeout_ms(timeout_ms);
        session.touch(current_ms);

        response.status = RtspStatusCode::ok;
        response.headers["Session"] = session.id() + ";timeout=" + std::to_string(session.timeout_ms() / 1000);

        RtspTransportSpec final_spec = spec;
        if (final_spec.transport == RtspLowerTransport::rtp_avp_udp) {
            if (final_spec.server_rtp_port < 0) {
                final_spec.server_rtp_port = 50000;
            }
            if (final_spec.server_rtcp_port < 0) {
                final_spec.server_rtcp_port = final_spec.server_rtp_port + 1;
            }
        }
        if (final_spec.transport == RtspLowerTransport::rtp_avp_tcp &&
            final_spec.interleaved_rtp_channel < 0) {
            final_spec.interleaved_rtp_channel = 0;
            final_spec.interleaved_rtcp_channel = 1;
        }
        response.headers["Transport"] = format_transport_header(final_spec);
        return finalize_and_return(response.status, "ok_setup");
    }

    if (request.method == RtspMethod::play ||
        request.method == RtspMethod::pause ||
        request.method == RtspMethod::record ||
        request.method == RtspMethod::teardown ||
        request.method == RtspMethod::get_parameter ||
        request.method == RtspMethod::set_parameter) {
        const std::string sid = session_header_value(request);
        auto iter = sessions_.find(sid);
        if (sid.empty()) {
            response.status = RtspStatusCode::session_not_found;
            return finalize_and_return(response.status, "method_missing_session");
        }
        if (iter == sessions_.end()) {
            set_session_lookup_error(sid, expired_sessions_, response);
            return finalize_and_return(response.status, response.status == RtspStatusCode::request_timeout ? "method_session_expired" : "method_session_not_found");
        }

        RtspSession &session = iter->second;
        if (!session.validate_cseq(request.cseq)) {
            response.status = RtspStatusCode::bad_request;
            return finalize_and_return(response.status, "method_bad_cseq");
        }

        if (request.method == RtspMethod::play) {
            double scale = 1.0;
            if (!parse_scale_header(request, scale)) {
                response.status = RtspStatusCode::parameter_not_understood;
                return finalize_and_return(response.status, "play_bad_scale");
            }
            if (!is_scale_supported(scale)) {
                return finalize_and_return(RtspStatusCode::method_not_valid_in_this_state, "play_unsupported_scale");
            }
            std::string range = "npt=0.000-";
            if (!parse_range_header(request, range)) {
                response.status = RtspStatusCode::parameter_not_understood;
                return finalize_and_return(response.status, "play_bad_range");
            }

            if (!session.on_play()) {
                return finalize_and_return(RtspStatusCode::method_not_valid_in_this_state, "play_invalid_state");
            }
            session.touch(current_ms);
            response.status = RtspStatusCode::ok;
            response.headers["Session"] = session.id();
            response.headers["Range"] = range;
            if (scale != 1.0) {
                std::ostringstream scale_oss;
                scale_oss << std::fixed << std::setprecision(3) << scale;
                response.headers["Scale"] = scale_oss.str();
            }
            const std::string rtp_info = build_rtp_info_for_tracks(session.tracks());
            if (!rtp_info.empty()) {
                response.headers["RTP-Info"] = rtp_info;
            }

            for (const std::string &track : session.tracks()) {
                RtspTransportSpec transport;
                if (!session.resolve_track_transport(track, transport)) {
                    continue;
                }
                if (transport.transport == RtspLowerTransport::rtp_avp_tcp) {
                    (void)queue_interleaved_report_locked(session.id(), track, false);
                } else if (transport.transport == RtspLowerTransport::rtp_avp_udp) {
                    (void)queue_udp_report_locked(session.id(), track, false);
                }
            }
            return finalize_and_return(response.status, "ok_play");
        }

        if (request.method == RtspMethod::pause) {
            if (!session.on_pause()) {
                response.status = RtspStatusCode::method_not_valid_in_this_state;
                return finalize_and_return(response.status, "pause_invalid_state");
            }
            session.touch(current_ms);
            response.status = RtspStatusCode::ok;
            response.headers["Session"] = session.id();
            return finalize_and_return(response.status, "ok_pause");
        }

        if (request.method == RtspMethod::record) {
            auto media_iter = media_bridges_.find(session.id());
            if (media_iter == media_bridges_.end()) {
                media_iter = media_bridges_.emplace(session.id(), SessionMediaBridge{}).first;
                media_iter->second.rtcp_session.set_local_ssrc(media_bridge_ssrc(session.id()));
            }

            ::yuan::net::rtc::RtcPacket synthetic_probe;
            synthetic_probe.ssrc = media_bridge_ssrc(session.id());
            synthetic_probe.sequence_number = static_cast<uint16_t>(request.cseq & 0xFFFF);
            synthetic_probe.timestamp = static_cast<uint32_t>(current_ms & 0xFFFFFFFFu);
            synthetic_probe.payload_type = 96;
            synthetic_probe.payload = {0x00};
            media_iter->second.rtp_manager.on_packet_received(synthetic_probe, current_ms);

            if (!session.on_record()) {
                response.status = RtspStatusCode::method_not_valid_in_this_state;
                return finalize_and_return(response.status, "record_invalid_state");
            }
            session.touch(current_ms);
            response.status = RtspStatusCode::ok;
            response.headers["Session"] = session.id();

            for (const std::string &track : session.tracks()) {
                RtspTransportSpec transport;
                if (!session.resolve_track_transport(track, transport)) {
                    continue;
                }
                if (transport.transport == RtspLowerTransport::rtp_avp_tcp) {
                    (void)queue_interleaved_report_locked(session.id(), track, true);
                } else if (transport.transport == RtspLowerTransport::rtp_avp_udp) {
                    (void)queue_udp_report_locked(session.id(), track, true);
                }
            }
            return finalize_and_return(response.status, "ok_record");
        }

        if (request.method == RtspMethod::teardown) {
            session.on_teardown();
            session.touch(current_ms);
            response.status = RtspStatusCode::ok;
            response.headers["Session"] = session.id();
            erase_prefixed_keys(feedback_rr_last_ms_, session.id() + "|");
            erase_prefixed_keys(feedback_sr_last_ms_, session.id() + "|");
            expired_sessions_.erase(session.id());
            session_connection_owner_.erase(session.id());
            media_bridges_.erase(session.id());
            sessions_.erase(session.id());
            return finalize_and_return(response.status, "ok_teardown");
        }

        uint64_t keepalive_timeout_ms = 0;
        if (request.method == RtspMethod::set_parameter &&
            parse_keepalive_timeout_ms(request, keepalive_timeout_ms)) {
            session.set_timeout_ms(keepalive_timeout_ms);
        }
        session.touch(current_ms);
        response.status = RtspStatusCode::ok;
        response.headers["Session"] = session.id() + ";timeout=" + std::to_string(session.timeout_ms() / 1000);
        return finalize_and_return(response.status, "ok_parameter");
    }

    return finalize_and_return(RtspStatusCode::method_not_allowed, "method_not_allowed");
}

coroutine::Task<void> RtspServer::handle_connection(AsyncConnectionContext ctx)
{
    const uint64_t connection_id = [&]() {
        std::lock_guard<std::mutex> lock(sessions_mutex_);
        return next_connection_id_++;
    }();

    auto write_pending_outbound = [&](const std::string &current_client_ip) -> coroutine::Task<bool> {
        while (true) {
            RtspOutboundPacket pending;
            {
                std::lock_guard<std::mutex> lock(sessions_mutex_);
                auto it = outbound_packets_.begin();
                for (; it != outbound_packets_.end(); ++it) {
                    if (it->transport == RtspOutboundTransport::interleaved_tcp) {
                        auto owner_it = session_connection_owner_.find(it->session_id);
                        if (owner_it == session_connection_owner_.end() || owner_it->second != connection_id) {
                            continue;
                        }
                    }
                    pending = std::move(*it);
                    outbound_packets_.erase(it);
                    break;
                }
                if (pending.bytes.empty()) {
                    break;
                }
            }

            if (pending.transport == RtspOutboundTransport::interleaved_tcp) {
                ::yuan::buffer::ByteBuffer out;
                out.append(std::string_view(pending.bytes));
                auto write_result = co_await ctx.write_async(out);
                if (write_result.status != coroutine::IoStatus::success) {
                    co_return false;
                }
                if (observability_config_.enable_log) {
                    LOG_INFO(
                        "[RTSP] outbound transport=interleaved session={} track={} rtcp={} channel={} bytes={} client={}",
                        pending.session_id,
                        pending.track_uri,
                        pending.is_rtcp,
                        pending.interleaved_channel,
                        pending.bytes.size(),
                        current_client_ip.empty() ? std::string("-") : current_client_ip);
                }
                {
                    std::lock_guard<std::mutex> lock(sessions_mutex_);
                    ++metrics_.outbound_interleaved_sent;
                }
                continue;
            }

            if (pending.transport == RtspOutboundTransport::udp_unicast) {
                if (pending.next_retry_after_ms != 0 && pending.next_retry_after_ms > now_ms()) {
                    std::lock_guard<std::mutex> lock(sessions_mutex_);
                    outbound_packets_.push_back(std::move(pending));
                    continue;
                }
                const bool ok = send_udp_payload(current_client_ip, pending.udp_remote_port, std::string_view(pending.bytes));
                bool should_retry = false;
                {
                    std::lock_guard<std::mutex> lock(sessions_mutex_);
                    if (ok) {
                        ++metrics_.outbound_udp_sent;
                    } else {
                        ++metrics_.outbound_udp_failed;
                        ++metrics_.outbound_udp_failed_by_track[pending.track_uri.empty() ? std::string("-") : pending.track_uri];
                        should_retry = pending.retry_count < pending.max_retries;
                        RtspAuditEvent event;
                        event.timestamp_ms = now_ms();
                        event.client_ip = current_client_ip;
                        event.session_id = pending.session_id;
                        event.method = "OUTBOUND";
                        event.status_code = 0;
                        event.action = should_retry ? "udp_retry" : "udp_drop";
                        event.result = should_retry ? "scheduled" : "dropped";
                        event.detail = "track=" + pending.track_uri + ",port=" + std::to_string(pending.udp_remote_port);
                        record_audit_event_locked(std::move(event));
                    }
                }

                if (!ok && should_retry) {
                    pending.retry_count += 1;
                    pending.next_retry_after_ms = now_ms() + udp_retry_backoff_ms(
                        pending.retry_count,
                        observability_config_.udp_retry_base_backoff_ms,
                        observability_config_.udp_retry_max_backoff_ms);
                    std::lock_guard<std::mutex> lock(sessions_mutex_);
                    ++metrics_.outbound_udp_retried;
                    outbound_packets_.push_back(std::move(pending));
                } else if (!ok) {
                    std::lock_guard<std::mutex> lock(sessions_mutex_);
                    ++metrics_.outbound_udp_dropped;
                }
                if (observability_config_.enable_log) {
                    LOG_INFO(
                        "[RTSP] outbound transport=udp session={} track={} rtcp={} remote={}:{} bytes={} ok={}",
                        pending.session_id,
                        pending.track_uri,
                        pending.is_rtcp,
                        current_client_ip.empty() ? std::string("-") : current_client_ip,
                        pending.udp_remote_port,
                        pending.bytes.size(),
                        ok);
                }
            }
        }
        co_return true;
    };

    RtspStreamFramer framer;

    while (ctx.is_connected()) {
        auto read_result = co_await ctx.read_awaiter(30000);
        if (read_result.status != coroutine::IoStatus::success) {
            break;
        }

        const auto bytes = read_result.data.readable_bytes();
        if (bytes == 0) {
            continue;
        }
        framer.push(std::string_view(read_result.data.read_ptr(), bytes));

        while (true) {
            RtspFrame frame;
            const RtspFrameParseResult parse_result = framer.pop(frame);
            if (parse_result == RtspFrameParseResult::need_more) {
                break;
            }

            if (parse_result == RtspFrameParseResult::malformed) {
                {
                    std::lock_guard<std::mutex> lock(sessions_mutex_);
                    ++metrics_.parse_errors;
                    ++metrics_.status_counts[static_cast<uint16_t>(RtspStatusCode::bad_request)];
                    ++metrics_.responses_4xx;

                    RtspAuditEvent event;
                    event.timestamp_ms = now_ms();
                    event.client_ip = ctx.get_remote_address().get_ip();
                    event.method = "FRAME";
                    event.status_code = static_cast<uint16_t>(RtspStatusCode::bad_request);
                    event.action = "frame_parse";
                    event.result = "malformed";
                    record_audit_event_locked(std::move(event));
                }

                RtspResponse bad_resp;
                bad_resp.status = RtspStatusCode::bad_request;
                bad_resp.cseq = 0;
                const std::string encoded = RtspParser::serialize_response(bad_resp);
                ::yuan::buffer::ByteBuffer out;
                out.append(std::string_view(encoded));
                auto write_result = co_await ctx.write_async(out);
                if (write_result.status != coroutine::IoStatus::success) {
                    break;
                }
                framer.clear();
                continue;
            }

            if (frame.kind == RtspFrameKind::interleaved) {
                const uint64_t arrival_ms = now_ms();
                const InterleavedFrameResult frame_result =
                    handle_interleaved_frame(frame.channel, std::string_view(frame.data), arrival_ms);
                (void)frame_result;

                if (!(co_await write_pending_outbound(ctx.get_remote_address().get_ip()))) {
                    break;
                }
                continue;
            }

            RtspRequest request;
            RtspResponse response;
            if (!RtspParser::parse_request(frame.data, request)) {
                {
                    std::lock_guard<std::mutex> lock(sessions_mutex_);
                    ++metrics_.parse_errors;
                    ++metrics_.status_counts[static_cast<uint16_t>(RtspStatusCode::bad_request)];
                    ++metrics_.responses_4xx;

                    RtspAuditEvent event;
                    event.timestamp_ms = now_ms();
                    event.client_ip = ctx.get_remote_address().get_ip();
                    event.method = "REQUEST";
                    event.status_code = static_cast<uint16_t>(RtspStatusCode::bad_request);
                    event.action = "request_parse";
                    event.result = "bad_request";
                    record_audit_event_locked(std::move(event));
                }

                response.status = RtspStatusCode::bad_request;
                response.cseq = 0;
            } else {
                response = handle_request_with_context(request, ctx.get_remote_address().get_ip(), now_ms(), connection_id);
            }

            const std::string encoded = RtspParser::serialize_response(response);
            ::yuan::buffer::ByteBuffer out;
            out.append(std::string_view(encoded));
            auto write_result = co_await ctx.write_async(out);
            if (write_result.status != coroutine::IoStatus::success) {
                break;
            }

            if (!(co_await write_pending_outbound(ctx.get_remote_address().get_ip()))) {
                break;
            }
        }
    }

    {
        std::lock_guard<std::mutex> lock(sessions_mutex_);
        for (auto it = session_connection_owner_.begin(); it != session_connection_owner_.end();) {
            if (it->second == connection_id) {
                it = session_connection_owner_.erase(it);
            } else {
                ++it;
            }
        }
        for (auto it = outbound_packets_.begin(); it != outbound_packets_.end();) {
            if (it->transport == RtspOutboundTransport::interleaved_tcp) {
                auto owner_it = session_connection_owner_.find(it->session_id);
                if (owner_it == session_connection_owner_.end()) {
                    it = outbound_packets_.erase(it);
                    continue;
                }
            }
            ++it;
        }
    }

    ctx.close();
    co_return;
}

} // namespace yuan::net::rtsp
