#include "rtsp_server.h"
#include "rtsp_parser.h"
#include "rtcp_packet.h"
#include "rtc_packet.h"
#include "base/utils/base64.h"
#include "buffer/byte_buffer.h"

#include "openssl/evp.h"

#include <chrono>
#include <algorithm>
#include <array>
#include <iostream>
#include <string>
#include <thread>
#include <cstdint>

using namespace yuan::net::rtsp;

namespace
{

int g_run = 0;
int g_pass = 0;
int g_fail = 0;

#define TEST_ASSERT(expr, msg)                                                                  \
    do {                                                                                         \
        if (!(expr)) {                                                                           \
            std::cout << "  FAIL: " << msg << " (line " << __LINE__ << ")\n";             \
            return false;                                                                        \
        }                                                                                        \
    } while (0)

#define RUN_TEST(func)                                                                           \
    do {                                                                                         \
        ++g_run;                                                                                 \
        std::cout << "Running " #func "...\n";                                               \
        if (func()) {                                                                            \
            ++g_pass;                                                                            \
            std::cout << "  PASS\n";                                                           \
        } else {                                                                                 \
            ++g_fail;                                                                            \
            std::cout << "  FAIL\n";                                                           \
        }                                                                                        \
    } while (0)

bool parse_request_or_fail(const std::string &raw, RtspRequest &out)
{
    if (!RtspParser::parse_request(raw, out)) {
        std::cout << "  FAIL: failed to parse request fixture\n";
        return false;
    }
    return true;
}

std::string md5_hex(std::string_view text)
{
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    if (!ctx) {
        return {};
    }
    unsigned char digest[EVP_MAX_MD_SIZE] = {0};
    unsigned int len = 0;
    const bool ok = EVP_DigestInit_ex(ctx, EVP_md5(), nullptr) == 1 &&
                    EVP_DigestUpdate(ctx, text.data(), text.size()) == 1 &&
                    EVP_DigestFinal_ex(ctx, digest, &len) == 1;
    EVP_MD_CTX_free(ctx);
    if (!ok) {
        return {};
    }

    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(len * 2);
    for (unsigned int i = 0; i < len; ++i) {
        out.push_back(kHex[(digest[i] >> 4) & 0x0F]);
        out.push_back(kHex[digest[i] & 0x0F]);
    }
    return out;
}

bool extract_digest_nonce_realm(const std::string &challenge, std::string &out_nonce, std::string &out_realm)
{
    const auto digest_pos = challenge.find("Digest ");
    if (digest_pos == std::string::npos) {
        return false;
    }

    auto extract_quoted = [&challenge](const std::string &key, std::string &out) -> bool {
        const auto pos = challenge.find(key + "=\"");
        if (pos == std::string::npos) {
            return false;
        }
        const std::size_t begin = pos + key.size() + 2;
        const auto end = challenge.find('"', begin);
        if (end == std::string::npos) {
            return false;
        }
        out = challenge.substr(begin, end - begin);
        return true;
    };

    return extract_quoted("nonce", out_nonce) && extract_quoted("realm", out_realm);
}

bool extract_digest_field(const std::string &challenge, const std::string &key, std::string &out_value)
{
    const auto digest_pos = challenge.find("Digest ");
    if (digest_pos == std::string::npos) {
        return false;
    }
    const auto pos = challenge.find(key + "=\"", digest_pos);
    if (pos == std::string::npos) {
        return false;
    }
    const std::size_t begin = pos + key.size() + 2;
    const auto end = challenge.find('"', begin);
    if (end == std::string::npos) {
        return false;
    }
    out_value = challenge.substr(begin, end - begin);
    return true;
}

std::string build_digest_authorization(
    const std::string &username,
    const std::string &password,
    const std::string &realm,
    const std::string &nonce,
    const std::string &method,
    const std::string &uri,
    const std::string &nc,
    const std::string &cnonce,
    const std::string &opaque = "")
{
    const std::string qop = "auth";
    const std::string ha1 = md5_hex(username + ":" + realm + ":" + password);
    const std::string ha2 = md5_hex(method + ":" + uri);
    const std::string response = md5_hex(ha1 + ":" + nonce + ":" + nc + ":" + cnonce + ":" + qop + ":" + ha2);
    std::string header = "Digest username=\"" + username + "\", realm=\"" + realm +
                         "\", nonce=\"" + nonce + "\", uri=\"" + uri + "\", response=\"" + response +
                         "\", qop=\"" + qop + "\", nc=\"" + nc + "\", cnonce=\"" + cnonce + "\"";
    if (!opaque.empty()) {
        header += ", opaque=\"" + opaque + "\"";
    }
    return header;
}

bool test_rtsp_server_cross_request_session_lifecycle()
{
    RtspServer server;

    RtspRequest setup_req;
    if (!parse_request_or_fail(
            "SETUP rtsp://example/live/track1 RTSP/1.0\r\n"
            "CSeq: 1\r\n"
            "Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n"
            "\r\n",
            setup_req)) {
        return false;
    }

    const RtspResponse setup_resp = server.handle_request(setup_req);
    TEST_ASSERT(setup_resp.status == RtspStatusCode::ok, "setup should succeed");
    TEST_ASSERT(server.session_count() == 1, "session_count should become 1 after setup");

    auto it = setup_resp.headers.find("Session");
    TEST_ASSERT(it != setup_resp.headers.end(), "setup should return session header");
    const std::string session_header = it->second;
    const auto sep = session_header.find(';');
    TEST_ASSERT(sep != std::string::npos, "session header should include timeout parameter");
    const std::string sid = session_header.substr(0, sep);

    RtspRequest play_req;
    if (!parse_request_or_fail(
            "PLAY rtsp://example/live RTSP/1.0\r\n"
            "CSeq: 2\r\n"
            "Session: " + sid + "\r\n"
            "\r\n",
            play_req)) {
        return false;
    }
    const RtspResponse play_resp = server.handle_request(play_req);
    TEST_ASSERT(play_resp.status == RtspStatusCode::ok, "play should succeed with existing session");
    TEST_ASSERT(play_resp.headers.find("RTP-Info") != play_resp.headers.end(), "play should include rtp-info");
    TEST_ASSERT(play_resp.headers.at("RTP-Info").find("track1") != std::string::npos, "rtp-info should include setup track uri");

    RtspRequest teardown_req;
    if (!parse_request_or_fail(
            "TEARDOWN rtsp://example/live RTSP/1.0\r\n"
            "CSeq: 3\r\n"
            "Session: " + sid + "\r\n"
            "\r\n",
            teardown_req)) {
        return false;
    }
    const RtspResponse teardown_resp = server.handle_request(teardown_req);
    TEST_ASSERT(teardown_resp.status == RtspStatusCode::ok, "teardown should succeed");
    TEST_ASSERT(server.session_count() == 0, "session_count should become 0 after teardown");
    return true;
}

bool test_rtsp_server_invalid_session_and_cseq_paths()
{
    RtspServer server;

    RtspRequest bad_play;
    if (!parse_request_or_fail(
            "PLAY rtsp://example/live RTSP/1.0\r\n"
            "CSeq: 10\r\n"
            "Session: missing\r\n"
            "\r\n",
            bad_play)) {
        return false;
    }
    const RtspResponse missing_resp = server.handle_request(bad_play);
    TEST_ASSERT(missing_resp.status == RtspStatusCode::session_not_found, "play with unknown session should return 454");

    RtspRequest setup_req;
    if (!parse_request_or_fail(
            "SETUP rtsp://example/live/track1 RTSP/1.0\r\n"
            "CSeq: 20\r\n"
            "Transport: RTP/AVP;unicast;client_port=4000-4001\r\n"
            "\r\n",
            setup_req)) {
        return false;
    }
    const RtspResponse setup_resp = server.handle_request(setup_req);
    TEST_ASSERT(setup_resp.status == RtspStatusCode::ok, "setup should succeed");

    const std::string session_header = setup_resp.headers.at("Session");
    const std::string sid = session_header.substr(0, session_header.find(';'));

    RtspRequest play_req_1;
    if (!parse_request_or_fail(
            "PLAY rtsp://example/live RTSP/1.0\r\n"
            "CSeq: 21\r\n"
            "Session: " + sid + "\r\n"
            "\r\n",
            play_req_1)) {
        return false;
    }
    const RtspResponse play_ok = server.handle_request(play_req_1);
    TEST_ASSERT(play_ok.status == RtspStatusCode::ok, "first play should succeed");

    RtspRequest play_dup_cseq;
    if (!parse_request_or_fail(
            "PLAY rtsp://example/live RTSP/1.0\r\n"
            "CSeq: 21\r\n"
            "Session: " + sid + "\r\n"
            "\r\n",
            play_dup_cseq)) {
        return false;
    }
    const RtspResponse play_dup_resp = server.handle_request(play_dup_cseq);
    TEST_ASSERT(play_dup_resp.status == RtspStatusCode::bad_request, "duplicate cseq should return 400");
    return true;
}

bool test_rtsp_server_expired_session_returns_timeout()
{
    RtspServer server;

    RtspRequest setup_req;
    if (!parse_request_or_fail(
            "SETUP rtsp://example/live/track1 RTSP/1.0\r\n"
            "CSeq: 200\r\n"
            "Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n"
            "Session: sid-expired;timeout=1\r\n"
            "\r\n",
            setup_req)) {
        return false;
    }
    const RtspResponse setup_resp = server.handle_request(setup_req);
    TEST_ASSERT(setup_resp.status == RtspStatusCode::ok, "setup should succeed");

    RtspRequest options_req;
    if (!parse_request_or_fail(
            "OPTIONS rtsp://example/live RTSP/1.0\r\n"
            "CSeq: 201\r\n"
            "\r\n",
            options_req)) {
        return false;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    const RtspResponse options_resp = server.handle_request(options_req);
    TEST_ASSERT(options_resp.status == RtspStatusCode::ok, "options should succeed");

    RtspRequest play_req;
    if (!parse_request_or_fail(
            "PLAY rtsp://example/live RTSP/1.0\r\n"
            "CSeq: 202\r\n"
            "Session: sid-expired\r\n"
            "\r\n",
            play_req)) {
        return false;
    }
    const RtspResponse play_resp = server.handle_request(play_req);
    TEST_ASSERT(play_resp.status == RtspStatusCode::request_timeout, "expired session should return 408");
    TEST_ASSERT(play_resp.headers.find("Session") != play_resp.headers.end(), "expired response should carry session id");
    TEST_ASSERT(play_resp.headers.at("Session") == "sid-expired", "expired session id should match");
    return true;
}

bool test_rtsp_server_transport_candidate_fallback()
{
    RtspServer server;

    RtspRequest setup_req;
    if (!parse_request_or_fail(
            "SETUP rtsp://example/live/track1 RTSP/1.0\r\n"
            "CSeq: 210\r\n"
            "Transport: RTP/AVP/TCP;multicast;interleaved=0-1, RTP/AVP/TCP;unicast;interleaved=2-3\r\n"
            "\r\n",
            setup_req)) {
        return false;
    }
    const RtspResponse setup_resp = server.handle_request(setup_req);
    TEST_ASSERT(setup_resp.status == RtspStatusCode::ok, "setup should choose second valid candidate");
    TEST_ASSERT(setup_resp.headers.find("Transport") != setup_resp.headers.end(), "transport should be returned");
    TEST_ASSERT(setup_resp.headers.at("Transport").find("interleaved=2-3") != std::string::npos,
                "selected transport should be second candidate");
    return true;
}

bool test_rtsp_server_transport_candidate_errors()
{
    RtspServer server;

    RtspRequest malformed_only_req;
    if (!parse_request_or_fail(
            "SETUP rtsp://example/live/track1 RTSP/1.0\r\n"
            "CSeq: 220\r\n"
            "Transport: RTP/AVP/TCP;unicast;interleaved=1-1\r\n"
            "\r\n",
            malformed_only_req)) {
        return false;
    }
    const RtspResponse malformed_only_resp = server.handle_request(malformed_only_req);
    TEST_ASSERT(malformed_only_resp.status == RtspStatusCode::parameter_not_understood,
                "invalid interleaved pair should return 451");

    RtspRequest unsupported_only_req;
    if (!parse_request_or_fail(
            "SETUP rtsp://example/live/track1 RTSP/1.0\r\n"
            "CSeq: 221\r\n"
            "Transport: RTP/AVP/TCP;multicast;interleaved=0-1\r\n"
            "\r\n",
            unsupported_only_req)) {
        return false;
    }
    const RtspResponse unsupported_only_resp = server.handle_request(unsupported_only_req);
    TEST_ASSERT(unsupported_only_resp.status == RtspStatusCode::unsupported_transport,
                "unsupported candidates should return 461");
    return true;
}

bool test_rtsp_server_basic_auth_challenge_and_success()
{
    RtspServer server;
    server.configure_basic_auth("camera-realm", "alice", "secret");

    RtspRequest unauth_setup;
    if (!parse_request_or_fail(
            "SETUP rtsp://example/live/track1 RTSP/1.0\r\n"
            "CSeq: 230\r\n"
            "Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n"
            "\r\n",
            unauth_setup)) {
        return false;
    }
    const RtspResponse unauth_resp = server.handle_request(unauth_setup);
    TEST_ASSERT(unauth_resp.status == RtspStatusCode::unauthorized, "missing auth should return 401");
    TEST_ASSERT(unauth_resp.headers.find("WWW-Authenticate") != unauth_resp.headers.end(), "challenge header required");
    TEST_ASSERT(unauth_resp.headers.at("WWW-Authenticate").find("Basic realm=\"camera-realm\"") != std::string::npos,
                "challenge should include configured realm");

    const std::string auth = "Basic " + ::yuan::base::util::base64_encode("alice:secret");
    RtspRequest auth_setup;
    if (!parse_request_or_fail(
            "SETUP rtsp://example/live/track1 RTSP/1.0\r\n"
            "CSeq: 231\r\n"
            "Authorization: " + auth + "\r\n"
            "Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n"
            "\r\n",
            auth_setup)) {
        return false;
    }
    const RtspResponse auth_resp = server.handle_request(auth_setup);
    TEST_ASSERT(auth_resp.status == RtspStatusCode::ok, "valid basic auth should pass");

    RtspRequest lowercase_auth_setup;
    if (!parse_request_or_fail(
            "SETUP rtsp://example/live/track2 RTSP/1.0\r\n"
            "CSeq: 232\r\n"
            "authorization: " + auth + "\r\n"
            "Session: sid-auth-lower\r\n"
            "Transport: RTP/AVP/TCP;unicast;interleaved=2-3\r\n"
            "\r\n",
            lowercase_auth_setup)) {
        return false;
    }
    const RtspResponse lowercase_auth_resp = server.handle_request(lowercase_auth_setup);
    TEST_ASSERT(lowercase_auth_resp.status == RtspStatusCode::ok, "lowercase authorization header should be accepted");

    RtspRequest options_req;
    if (!parse_request_or_fail(
            "OPTIONS rtsp://example/live RTSP/1.0\r\n"
            "CSeq: 233\r\n"
            "\r\n",
            options_req)) {
        return false;
    }
    const RtspResponse options_resp = server.handle_request(options_req);
    TEST_ASSERT(options_resp.status == RtspStatusCode::ok, "options should remain auth-exempt");

    RtspRequest wrong_auth_setup;
    if (!parse_request_or_fail(
            "SETUP rtsp://example/live/track3 RTSP/1.0\r\n"
            "CSeq: 234\r\n"
            "Authorization: Basic bWF0cml4Ondyb25n\r\n"
            "Transport: RTP/AVP/TCP;unicast;interleaved=4-5\r\n"
            "\r\n",
            wrong_auth_setup)) {
        return false;
    }
    const RtspResponse wrong_auth_resp = server.handle_request(wrong_auth_setup);
    TEST_ASSERT(wrong_auth_resp.status == RtspStatusCode::unauthorized, "wrong credentials should return 401");
    TEST_ASSERT(wrong_auth_resp.headers.find("WWW-Authenticate") != wrong_auth_resp.headers.end(),
                "wrong credentials should still include challenge");
    return true;
}

bool test_rtsp_server_digest_auth_challenge_and_success()
{
    RtspServer server;
    server.configure_digest_auth("digest-realm", "bob", "s3cr3t");

    RtspRequest unauth_setup;
    if (!parse_request_or_fail(
            "SETUP rtsp://example/live/track1 RTSP/1.0\r\n"
            "CSeq: 280\r\n"
            "Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n"
            "\r\n",
            unauth_setup)) {
        return false;
    }
    const RtspResponse unauth_resp = server.handle_request(unauth_setup);
    TEST_ASSERT(unauth_resp.status == RtspStatusCode::unauthorized, "missing digest should return 401");
    TEST_ASSERT(unauth_resp.headers.find("WWW-Authenticate") != unauth_resp.headers.end(), "digest challenge required");

    std::string nonce;
    std::string realm;
    TEST_ASSERT(extract_digest_nonce_realm(unauth_resp.headers.at("WWW-Authenticate"), nonce, realm),
                "digest challenge should expose nonce and realm");
    TEST_ASSERT(realm == "digest-realm", "digest realm should match config");
    std::string opaque;
    TEST_ASSERT(extract_digest_field(unauth_resp.headers.at("WWW-Authenticate"), "opaque", opaque),
                "digest challenge should expose opaque");

    const std::string uri = "rtsp://example/live/track1";
    const std::string auth = build_digest_authorization(
        "bob", "s3cr3t", realm, nonce, "SETUP", uri, "00000001", "abcdef123456", opaque);

    RtspRequest digest_setup;
    if (!parse_request_or_fail(
            "SETUP rtsp://example/live/track1 RTSP/1.0\r\n"
            "CSeq: 281\r\n"
            "Authorization: " + auth + "\r\n"
            "Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n"
            "\r\n",
            digest_setup)) {
        return false;
    }
    const RtspResponse digest_ok = server.handle_request(digest_setup);
    TEST_ASSERT(digest_ok.status == RtspStatusCode::ok, "valid digest should pass");

    RtspRequest digest_bad;
    if (!parse_request_or_fail(
            "SETUP rtsp://example/live/track2 RTSP/1.0\r\n"
            "CSeq: 282\r\n"
            "Authorization: Digest username=\"bob\", realm=\"digest-realm\", nonce=\"bad\", uri=\"rtsp://example/live/track2\", response=\"deadbeef\", qop=\"auth\", nc=\"00000001\", cnonce=\"c\"\r\n"
            "Transport: RTP/AVP/TCP;unicast;interleaved=2-3\r\n"
            "\r\n",
            digest_bad)) {
        return false;
    }
    const RtspResponse digest_bad_resp = server.handle_request(digest_bad);
    TEST_ASSERT(digest_bad_resp.status == RtspStatusCode::unauthorized, "bad nonce digest should fail");

    RtspRequest stale_req;
    if (!parse_request_or_fail(
            "SETUP rtsp://example/live/track3 RTSP/1.0\r\n"
            "CSeq: 283\r\n"
            "Authorization: Digest username=\"bob\", realm=\"digest-realm\", nonce=\"bad\", opaque=\"x\", uri=\"rtsp://example/live/track3\", response=\"deadbeef\", qop=\"auth\", nc=\"00000001\", cnonce=\"c\", algorithm=MD5\r\n"
            "Transport: RTP/AVP/TCP;unicast;interleaved=4-5\r\n"
            "\r\n",
            stale_req)) {
        return false;
    }
    const RtspResponse stale_resp = server.handle_request(stale_req);
    TEST_ASSERT(stale_resp.status == RtspStatusCode::unauthorized, "stale digest should still return 401");
    TEST_ASSERT(stale_resp.headers.find("WWW-Authenticate") != stale_resp.headers.end(),
                "stale digest should include challenge");
    TEST_ASSERT(stale_resp.headers.at("WWW-Authenticate").find("stale=TRUE") != std::string::npos,
                "stale challenge should include stale flag");

    std::string replay_nonce;
    std::string replay_realm;
    std::string replay_opaque;
    TEST_ASSERT(extract_digest_nonce_realm(stale_resp.headers.at("WWW-Authenticate"), replay_nonce, replay_realm),
                "replay challenge should expose nonce/realm");
    TEST_ASSERT(extract_digest_field(stale_resp.headers.at("WWW-Authenticate"), "opaque", replay_opaque),
                "replay challenge should expose opaque");

    const std::string replay_uri = "rtsp://example/live/track3";
    const std::string replay_auth = build_digest_authorization(
        "bob", "s3cr3t", replay_realm, replay_nonce, "SETUP", replay_uri, "00000005", "cnc", replay_opaque);

    RtspRequest replay_ok_req;
    if (!parse_request_or_fail(
            "SETUP rtsp://example/live/track3 RTSP/1.0\r\n"
            "CSeq: 284\r\n"
            "Authorization: " + replay_auth + "\r\n"
            "Transport: RTP/AVP/TCP;unicast;interleaved=6-7\r\n"
            "\r\n",
            replay_ok_req)) {
        return false;
    }
    const RtspResponse replay_ok_resp = server.handle_request(replay_ok_req);
    TEST_ASSERT(replay_ok_resp.status == RtspStatusCode::ok, "fresh digest with higher nc should pass");

    RtspRequest replay_dup_req;
    if (!parse_request_or_fail(
            "SETUP rtsp://example/live/track4 RTSP/1.0\r\n"
            "CSeq: 285\r\n"
            "Authorization: " + replay_auth + "\r\n"
            "Transport: RTP/AVP/TCP;unicast;interleaved=8-9\r\n"
            "\r\n",
            replay_dup_req)) {
        return false;
    }
    const RtspResponse replay_dup_resp = server.handle_request(replay_dup_req);
    TEST_ASSERT(replay_dup_resp.status == RtspStatusCode::unauthorized, "reused nc digest should be rejected");

    std::string sha_nonce;
    std::string sha_realm;
    std::string sha_opaque;
    TEST_ASSERT(extract_digest_nonce_realm(replay_dup_resp.headers.at("WWW-Authenticate"), sha_nonce, sha_realm),
                "sha challenge should expose nonce/realm");
    TEST_ASSERT(extract_digest_field(replay_dup_resp.headers.at("WWW-Authenticate"), "opaque", sha_opaque),
                "sha challenge should expose opaque");

    auto digest_hex_with_algorithm = [](std::string_view text, const EVP_MD *md) -> std::string {
        EVP_MD_CTX *ctx = EVP_MD_CTX_new();
        if (!ctx) {
            return {};
        }
        unsigned char digest[EVP_MAX_MD_SIZE] = {0};
        unsigned int len = 0;
        const bool ok = EVP_DigestInit_ex(ctx, md, nullptr) == 1 &&
                        EVP_DigestUpdate(ctx, text.data(), text.size()) == 1 &&
                        EVP_DigestFinal_ex(ctx, digest, &len) == 1;
        EVP_MD_CTX_free(ctx);
        if (!ok) {
            return {};
        }
        static constexpr char kHex[] = "0123456789abcdef";
        std::string out;
        out.reserve(len * 2);
        for (unsigned int i = 0; i < len; ++i) {
            out.push_back(kHex[(digest[i] >> 4) & 0x0F]);
            out.push_back(kHex[digest[i] & 0x0F]);
        }
        return out;
    };

    const std::string sha_uri = "rtsp://example/live/track5";
    const std::string sha_qop = "auth";
    const std::string sha_nc = "00000001";
    const std::string sha_cnonce = "sha-cnonce";
    const std::string ha1 = digest_hex_with_algorithm("bob:" + sha_realm + ":s3cr3t", EVP_sha256());
    const std::string ha2 = digest_hex_with_algorithm("SETUP:" + sha_uri, EVP_sha256());
    const std::string sha_response = digest_hex_with_algorithm(
        ha1 + ":" + sha_nonce + ":" + sha_nc + ":" + sha_cnonce + ":" + sha_qop + ":" + ha2,
        EVP_sha256());
    const std::string sha_auth = "Digest username=\"bob\", realm=\"" + sha_realm +
                                 "\", nonce=\"" + sha_nonce +
                                 "\", opaque=\"" + sha_opaque +
                                 "\", uri=\"" + sha_uri +
                                 "\", response=\"" + sha_response +
                                 "\", qop=\"auth\", nc=\"" + sha_nc +
                                 "\", cnonce=\"" + sha_cnonce +
                                 "\", algorithm=SHA-256";

    RtspRequest sha_req;
    if (!parse_request_or_fail(
            "SETUP rtsp://example/live/track5 RTSP/1.0\r\n"
            "CSeq: 286\r\n"
            "Authorization: " + sha_auth + "\r\n"
            "Transport: RTP/AVP/TCP;unicast;interleaved=10-11\r\n"
            "\r\n",
            sha_req)) {
        return false;
    }
    const RtspResponse sha_resp = server.handle_request(sha_req);
    TEST_ASSERT(sha_resp.status == RtspStatusCode::ok, "sha-256 digest should pass");
    return true;
}

bool test_rtsp_server_acl_and_rate_limit()
{
    RtspServer server;

    RtspAclConfig acl;
    acl.enabled = true;
    acl.default_allow = true;
    acl.deny_uri_prefixes.push_back("rtsp://example/deny");
    server.configure_acl(acl);

    RtspRequest denied_req;
    if (!parse_request_or_fail(
            "DESCRIBE rtsp://example/deny/live RTSP/1.0\r\n"
            "CSeq: 290\r\n"
            "\r\n",
            denied_req)) {
        return false;
    }
    const RtspResponse denied_resp = server.handle_request(denied_req);
    TEST_ASSERT(denied_resp.status == RtspStatusCode::forbidden, "deny prefix should return 403");

    RtspRequest allowed_req;
    if (!parse_request_or_fail(
            "DESCRIBE rtsp://example/live RTSP/1.0\r\n"
            "CSeq: 291\r\n"
            "\r\n",
            allowed_req)) {
        return false;
    }
    const RtspResponse allowed_resp = server.handle_request(allowed_req);
    TEST_ASSERT(allowed_resp.status == RtspStatusCode::ok, "allowed route should pass");

    RtspRateLimitConfig rl;
    rl.enabled = true;
    rl.max_requests = 2;
    rl.window_ms = 10000;
    server.configure_rate_limit(rl);

    RtspRequest req1;
    RtspRequest req2;
    RtspRequest req3;
    if (!parse_request_or_fail("OPTIONS rtsp://example/live RTSP/1.0\r\nCSeq: 292\r\n\r\n", req1) ||
        !parse_request_or_fail("OPTIONS rtsp://example/live RTSP/1.0\r\nCSeq: 293\r\n\r\n", req2) ||
        !parse_request_or_fail("OPTIONS rtsp://example/live RTSP/1.0\r\nCSeq: 294\r\n\r\n", req3)) {
        return false;
    }

    const RtspResponse r1 = server.handle_request(req1);
    const RtspResponse r2 = server.handle_request(req2);
    const RtspResponse r3 = server.handle_request(req3);
    TEST_ASSERT(r1.status == RtspStatusCode::ok, "rate limit first request should pass");
    TEST_ASSERT(r2.status == RtspStatusCode::ok, "rate limit second request should pass");
    TEST_ASSERT(r3.status == RtspStatusCode::too_many_requests, "rate limit overflow should return 429");

    const SecurityStatsSnapshot sec = server.security_stats_snapshot();
    TEST_ASSERT(sec.acl_denied >= 1, "acl denied counter should increase");
    TEST_ASSERT(sec.rate_limited >= 1, "rate limited counter should increase");
    return true;
}

bool test_rtsp_server_observability_metrics_and_audit()
{
    RtspServer server;

    RtspObservabilityConfig obs;
    obs.enable_log = false;
    obs.enable_audit = true;
    obs.max_audit_events = 8;
    server.configure_observability(obs);

    RtspRequest options_req;
    if (!parse_request_or_fail(
            "OPTIONS rtsp://example/obs RTSP/1.0\r\n"
            "CSeq: 500\r\n"
            "\r\n",
            options_req)) {
        return false;
    }
    const RtspResponse options_resp = server.handle_request(options_req);
    TEST_ASSERT(options_resp.status == RtspStatusCode::ok, "options should succeed");

    RtspRequest bad_setup;
    if (!parse_request_or_fail(
            "SETUP rtsp://example/obs/track1 RTSP/1.0\r\n"
            "CSeq: 501\r\n"
            "\r\n",
            bad_setup)) {
        return false;
    }
    const RtspResponse bad_setup_resp = server.handle_request(bad_setup);
    TEST_ASSERT(bad_setup_resp.status == RtspStatusCode::unsupported_transport,
                "setup without transport should fail");

    const RtspMetricsSnapshot metrics = server.metrics_snapshot();
    TEST_ASSERT(metrics.requests_total >= 2, "metrics should count requests");
    TEST_ASSERT(metrics.responses_2xx >= 1, "metrics should count success responses");
    TEST_ASSERT(metrics.responses_4xx >= 1, "metrics should count client errors");
    TEST_ASSERT(metrics.method_counts.find("OPTIONS") != metrics.method_counts.end(),
                "method metrics should include OPTIONS");
    TEST_ASSERT(metrics.status_counts.find(static_cast<uint16_t>(RtspStatusCode::ok)) != metrics.status_counts.end(),
                "status metrics should include 200");
    TEST_ASSERT(metrics.outbound_interleaved_sent == 0,
                "outbound interleaved metric should start at zero in this scenario");
    TEST_ASSERT(metrics.outbound_udp_sent == 0,
                "outbound udp metric should start at zero in this scenario");
    TEST_ASSERT(metrics.outbound_udp_failed == 0,
                "outbound udp failed metric should start at zero in this scenario");

    const auto audits = server.recent_audit_events(8);
    TEST_ASSERT(!audits.empty(), "audit events should be collected");
    bool found_options = false;
    bool found_bad_setup = false;
    for (const auto &event : audits) {
        if (event.method == "OPTIONS" && event.status_code == static_cast<uint16_t>(RtspStatusCode::ok)) {
            found_options = true;
        }
        if (event.method == "SETUP" && event.status_code == static_cast<uint16_t>(RtspStatusCode::unsupported_transport)) {
            found_bad_setup = true;
        }
    }
    TEST_ASSERT(found_options, "audit should contain options success");
    TEST_ASSERT(found_bad_setup, "audit should contain setup transport error");
    return true;
}

bool test_rtsp_server_flush_udp_outbound_packets_metrics()
{
    RtspServer server;

    RtspRequest setup_req;
    if (!parse_request_or_fail(
            "SETUP rtsp://example/live/track7 RTSP/1.0\r\n"
            "CSeq: 510\r\n"
            "Session: sid-flush-udp\r\n"
            "Transport: RTP/AVP;unicast;client_port=5200-5201\r\n"
            "\r\n",
            setup_req)) {
        return false;
    }
    const RtspResponse setup_resp = server.handle_request(setup_req);
    TEST_ASSERT(setup_resp.status == RtspStatusCode::ok, "udp setup should succeed");

    RtspRequest play_req;
    if (!parse_request_or_fail(
            "PLAY rtsp://example/live RTSP/1.0\r\n"
            "CSeq: 511\r\n"
            "Session: sid-flush-udp\r\n"
            "\r\n",
            play_req)) {
        return false;
    }
    const RtspResponse play_resp = server.handle_request(play_req);
    TEST_ASSERT(play_resp.status == RtspStatusCode::ok, "play should succeed");
    TEST_ASSERT(server.outbound_packet_count() >= 1, "udp feedback packet should be queued");

    const std::size_t flushed = server.flush_udp_outbound_packets("127.0.0.1", 8);
    TEST_ASSERT(flushed >= 1, "flush api should process queued udp packets");

    const RtspMetricsSnapshot metrics = server.metrics_snapshot();
    TEST_ASSERT(metrics.outbound_udp_sent >= 1, "udp sent metric should increase");
    TEST_ASSERT(metrics.outbound_udp_failed == 0, "udp failed metric should remain zero on localhost");
    return true;
}

bool test_rtsp_server_udp_failure_bucket_and_audit_detail()
{
    RtspServer server;
    RtspObservabilityConfig obs;
    obs.enable_log = false;
    obs.enable_audit = true;
    obs.max_audit_events = 32;
    server.configure_observability(obs);

    RtspRequest setup_req;
    if (!parse_request_or_fail(
            "SETUP rtsp://example/live/track-fail RTSP/1.0\r\n"
            "CSeq: 520\r\n"
            "Session: sid-udp-fail\r\n"
            "Transport: RTP/AVP;unicast;client_port=5300-5301\r\n"
            "\r\n",
            setup_req)) {
        return false;
    }
    const RtspResponse setup_resp = server.handle_request(setup_req);
    TEST_ASSERT(setup_resp.status == RtspStatusCode::ok, "udp setup should succeed");

    RtspRequest play_req;
    if (!parse_request_or_fail(
            "PLAY rtsp://example/live RTSP/1.0\r\n"
            "CSeq: 521\r\n"
            "Session: sid-udp-fail\r\n"
            "\r\n",
            play_req)) {
        return false;
    }
    const RtspResponse play_resp = server.handle_request(play_req);
    TEST_ASSERT(play_resp.status == RtspStatusCode::ok, "play should succeed");

    const std::size_t flushed = server.flush_udp_outbound_packets("bad-ip", 8);
    TEST_ASSERT(flushed >= 1, "flush should consume queued udp packet even on send failure");

    const RtspMetricsSnapshot metrics = server.metrics_snapshot();
    TEST_ASSERT(metrics.outbound_udp_failed >= 1, "udp failed metric should increase");
    const auto fail_it = metrics.outbound_udp_failed_by_track.find("rtsp://example/live/track-fail");
    TEST_ASSERT(fail_it != metrics.outbound_udp_failed_by_track.end(), "failed-by-track bucket should exist");
    TEST_ASSERT(fail_it->second >= 1, "failed-by-track counter should increase");

    const auto audits = server.recent_audit_events(32);
    bool found_udp_fail_audit = false;
    for (const auto &event : audits) {
        if (event.method == "OUTBOUND" && (event.action == "udp_retry" || event.action == "udp_drop") &&
            event.detail.find("track=rtsp://example/live/track-fail") != std::string::npos) {
            found_udp_fail_audit = true;
            break;
        }
    }
    TEST_ASSERT(found_udp_fail_audit, "audit should record udp send failure detail");
    return true;
}

bool test_rtsp_server_udp_retry_and_drop_metrics()
{
    RtspServer server;

    RtspRequest setup_req;
    if (!parse_request_or_fail(
            "SETUP rtsp://example/live/track-retry RTSP/1.0\r\n"
            "CSeq: 530\r\n"
            "Session: sid-udp-retry\r\n"
            "Transport: RTP/AVP;unicast;client_port=5400-5401\r\n"
            "\r\n",
            setup_req)) {
        return false;
    }
    const RtspResponse setup_resp = server.handle_request(setup_req);
    TEST_ASSERT(setup_resp.status == RtspStatusCode::ok, "udp setup should succeed");

    RtspRequest play_req;
    if (!parse_request_or_fail(
            "PLAY rtsp://example/live RTSP/1.0\r\n"
            "CSeq: 531\r\n"
            "Session: sid-udp-retry\r\n"
            "\r\n",
            play_req)) {
        return false;
    }
    const RtspResponse play_resp = server.handle_request(play_req);
    TEST_ASSERT(play_resp.status == RtspStatusCode::ok, "play should succeed");

    const RtspMetricsSnapshot before = server.metrics_snapshot();
    for (int i = 0; i < 12; ++i) {
        (void)server.flush_udp_outbound_packets("bad-ip", 8);
        std::this_thread::sleep_for(std::chrono::milliseconds(35));
    }

    const RtspMetricsSnapshot after = server.metrics_snapshot();
    TEST_ASSERT(after.outbound_udp_failed > before.outbound_udp_failed,
                "udp failed metric should increase on repeated send failures");
    TEST_ASSERT(after.outbound_udp_retried >= 1,
                "udp retried metric should increase");
    TEST_ASSERT(after.outbound_udp_dropped >= 1,
                "udp dropped metric should increase after max retries");
    return true;
}

bool test_rtsp_server_udp_retry_config_zero_drop_immediate()
{
    RtspServer server;
    RtspObservabilityConfig obs;
    obs.enable_log = false;
    obs.enable_audit = true;
    obs.max_audit_events = 32;
    obs.udp_retry_max_retries = 0;
    obs.udp_retry_base_backoff_ms = 1;
    obs.udp_retry_max_backoff_ms = 1;
    server.configure_observability(obs);

    RtspRequest setup_req;
    if (!parse_request_or_fail(
            "SETUP rtsp://example/live/track-zero-retry RTSP/1.0\r\n"
            "CSeq: 540\r\n"
            "Session: sid-udp-zero-retry\r\n"
            "Transport: RTP/AVP;unicast;client_port=5500-5501\r\n"
            "\r\n",
            setup_req)) {
        return false;
    }
    const RtspResponse setup_resp = server.handle_request(setup_req);
    TEST_ASSERT(setup_resp.status == RtspStatusCode::ok, "udp setup should succeed");

    RtspRequest play_req;
    if (!parse_request_or_fail(
            "PLAY rtsp://example/live RTSP/1.0\r\n"
            "CSeq: 541\r\n"
            "Session: sid-udp-zero-retry\r\n"
            "\r\n",
            play_req)) {
        return false;
    }
    const RtspResponse play_resp = server.handle_request(play_req);
    TEST_ASSERT(play_resp.status == RtspStatusCode::ok, "play should succeed");

    const RtspMetricsSnapshot before = server.metrics_snapshot();
    (void)server.flush_udp_outbound_packets("bad-ip", 8);
    const RtspMetricsSnapshot after = server.metrics_snapshot();
    TEST_ASSERT(after.outbound_udp_failed > before.outbound_udp_failed,
                "udp failed metric should increase");
    TEST_ASSERT(after.outbound_udp_retried == before.outbound_udp_retried,
                "udp retried metric should not increase when max_retries=0");
    TEST_ASSERT(after.outbound_udp_dropped > before.outbound_udp_dropped,
                "udp dropped metric should increase immediately when max_retries=0");
    return true;
}

bool test_rtsp_server_media_bridge_inject_and_rr()
{
    RtspServer server;

    RtspRequest setup_req;
    if (!parse_request_or_fail(
            "SETUP rtsp://example/live/track1 RTSP/1.0\r\n"
            "CSeq: 240\r\n"
            "Session: sid-bridge\r\n"
            "Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n"
            "\r\n",
            setup_req)) {
        return false;
    }
    const RtspResponse setup_resp = server.handle_request(setup_req);
    TEST_ASSERT(setup_resp.status == RtspStatusCode::ok, "setup should succeed");

    ::yuan::net::rtc::RtcPacket packet;
    packet.ssrc = 123456;
    packet.sequence_number = 77;
    packet.timestamp = 9000;
    packet.payload_type = 96;
    packet.payload = {0x11, 0x22, 0x33};

    TEST_ASSERT(server.inject_rtp_packet("sid-bridge", packet, 2000), "inject rtp should succeed");

    ::yuan::net::rtcp::RtcpPacket rr;
    TEST_ASSERT(server.build_receiver_report("sid-bridge", rr), "build rr should succeed");
    TEST_ASSERT(rr.kind == ::yuan::net::rtcp::RtcpPacket::Kind::receiver_report, "rr kind should be receiver report");
    TEST_ASSERT(rr.receiver_report.report_blocks.size() == 1, "rr should contain one report block");
    TEST_ASSERT(rr.receiver_report.report_blocks[0].ssrc == 123456, "rr block should match injected ssrc");

    ::yuan::net::rtcp::RtcpPacket sr;
    TEST_ASSERT(server.build_sender_report("sid-bridge", sr), "build sr should succeed");
    TEST_ASSERT(sr.kind == ::yuan::net::rtcp::RtcpPacket::Kind::sender_report, "sr kind should be sender report");

    RtspMediaBridgeSnapshot snap;
    TEST_ASSERT(server.media_bridge_snapshot("sid-bridge", snap), "bridge snapshot should succeed");
    TEST_ASSERT(snap.rtp_session_count >= 1, "snapshot should report at least one RTP session");
    TEST_ASSERT(snap.rtcp.rr_reports_built >= 1, "snapshot should report rr count");
    TEST_ASSERT(snap.rtcp.sr_reports_built >= 1, "snapshot should report sr count");

    RtspInterleavedFrame missing_track;
    TEST_ASSERT(!server.build_interleaved_rtp_frame("sid-bridge", "rtsp://example/live/no-track", packet, missing_track),
                "interleaved rtp frame should fail for missing track mapping");
    return true;
}

bool test_rtsp_server_rtcp_sender_report_updates_bridge()
{
    RtspServer server;

    RtspRequest setup_req;
    if (!parse_request_or_fail(
            "SETUP rtsp://example/live/track1 RTSP/1.0\r\n"
            "CSeq: 250\r\n"
            "Session: sid-rtcp\r\n"
            "Transport: RTP/AVP/TCP;unicast;interleaved=10-11\r\n"
            "\r\n",
            setup_req)) {
        return false;
    }
    const RtspResponse setup_resp = server.handle_request(setup_req);
    TEST_ASSERT(setup_resp.status == RtspStatusCode::ok, "setup should succeed");

    ::yuan::net::rtc::RtcPacket rtp;
    rtp.ssrc = 456789;
    rtp.sequence_number = 1;
    rtp.timestamp = 1000;
    rtp.payload_type = 96;
    rtp.payload = {0x01};
    TEST_ASSERT(server.inject_rtp_packet("sid-rtcp", rtp, 5000), "inject rtp should succeed");

    ::yuan::net::rtcp::RtcpPacket sr;
    sr.kind = ::yuan::net::rtcp::RtcpPacket::Kind::sender_report;
    sr.sender_report.ssrc = 456789;
    sr.sender_report.ntp_timestamp = 0x1122334455667788ULL;
    sr.sender_report.rtp_timestamp = 12345;
    sr.sender_report.packet_count = 99;
    sr.sender_report.octet_count = 2048;

    ::yuan::buffer::ByteBuffer sr_buf;
    TEST_ASSERT(sr.serialize(sr_buf), "sender report should serialize");

    const auto rtcp_res = server.handle_interleaved_frame(
        11,
        std::string_view(sr_buf.read_ptr(), sr_buf.readable_bytes()),
        9100);
    TEST_ASSERT(rtcp_res == RtspServer::InterleavedFrameResult::handled_rtcp,
                "interleaved sender report should be handled");

    ::yuan::net::rtcp::RtcpPacket rr;
    TEST_ASSERT(server.build_receiver_report("sid-rtcp", rr), "receiver report should still build after SR payload prepared");
    TEST_ASSERT(rr.kind == ::yuan::net::rtcp::RtcpPacket::Kind::receiver_report, "rr kind should be receiver report");
    TEST_ASSERT(rr.receiver_report.report_blocks.size() == 1, "rr should contain report block after sr");
    TEST_ASSERT(rr.receiver_report.report_blocks[0].last_sr != 0, "rr should carry last_sr after sr activity");

    RtspMediaBridgeSnapshot snap;
    TEST_ASSERT(server.media_bridge_snapshot("sid-rtcp", snap), "snapshot should succeed");
    TEST_ASSERT(snap.rtcp.has_sender_activity, "snapshot should indicate sender activity");
    TEST_ASSERT(snap.rtcp.last_sr_lsr == rr.receiver_report.report_blocks[0].last_sr,
                "snapshot lsr should match rr block");
    TEST_ASSERT(snap.rtcp.last_sr_delay_65536 == rr.receiver_report.report_blocks[0].delay_since_last_sr,
                "snapshot dlsr should match rr block");

    ::yuan::net::rtcp::RtcpPacket sr_out;
    TEST_ASSERT(server.build_sender_report("sid-rtcp", sr_out), "sender report build should succeed");
    TEST_ASSERT(sr_out.kind == ::yuan::net::rtcp::RtcpPacket::Kind::sender_report, "sender report kind should match");
    TEST_ASSERT(sr_out.sender_report.ntp_timestamp == sr.sender_report.ntp_timestamp,
                "sender report should keep latest sender ntp");
    TEST_ASSERT(sr_out.sender_report.packet_count == sr.sender_report.packet_count,
                "sender report should keep latest sender packet count");

    ::yuan::net::rtc::RtcPacket outbound_rtp;
    outbound_rtp.ssrc = 456789;
    outbound_rtp.sequence_number = 2;
    outbound_rtp.timestamp = 2000;
    outbound_rtp.payload_type = 96;
    outbound_rtp.payload = {0xAB, 0xCD};

    RtspInterleavedFrame rtp_frame;
    TEST_ASSERT(server.build_interleaved_rtp_frame("sid-rtcp", "rtsp://example/live/track1", outbound_rtp, rtp_frame),
                "rtp interleaved frame should build");
    TEST_ASSERT(rtp_frame.channel == 10, "rtp frame should use track rtp channel");
    TEST_ASSERT(rtp_frame.bytes.size() > 4, "rtp frame should include payload");
    TEST_ASSERT(static_cast<unsigned char>(rtp_frame.bytes[0]) == static_cast<unsigned char>('$'),
                "rtp frame should start with interleaved magic");

    RtspInterleavedFrame rr_frame;
    TEST_ASSERT(server.build_interleaved_receiver_report_frame("sid-rtcp", "rtsp://example/live/track1", rr_frame),
                "rr interleaved frame should build");
    TEST_ASSERT(rr_frame.channel == 11, "rr frame should use track rtcp channel");
    TEST_ASSERT(rr_frame.bytes.size() > 4, "rr frame should include serialized rtcp");

    RtspInterleavedFrame sr_frame;
    TEST_ASSERT(server.build_interleaved_sender_report_frame("sid-rtcp", "rtsp://example/live/track1", sr_frame),
                "sr interleaved frame should build");
    TEST_ASSERT(sr_frame.channel == 11, "sr frame should use track rtcp channel");
    TEST_ASSERT(sr_frame.bytes.size() > 4, "sr frame should include serialized rtcp");
    return true;
}

bool test_rtsp_server_interleaved_frame_result_paths()
{
    RtspServer server;

    RtspRequest setup_req;
    if (!parse_request_or_fail(
            "SETUP rtsp://example/live/track1 RTSP/1.0\r\n"
            "CSeq: 260\r\n"
            "Session: sid-if\r\n"
            "Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n"
            "\r\n",
            setup_req)) {
        return false;
    }
    const RtspResponse setup_resp = server.handle_request(setup_req);
    TEST_ASSERT(setup_resp.status == RtspStatusCode::ok, "setup should succeed");

    ::yuan::net::rtc::RtcPacket rtp;
    rtp.ssrc = 777;
    rtp.sequence_number = 10;
    rtp.timestamp = 1234;
    rtp.payload_type = 96;
    rtp.payload = {0xAA};
    ::yuan::buffer::ByteBuffer rtp_buf;
    TEST_ASSERT(rtp.serialize(rtp_buf), "rtp should serialize");

    const auto handled_rtp = server.handle_interleaved_frame(
        0,
        std::string_view(rtp_buf.read_ptr(), rtp_buf.readable_bytes()),
        9000);
    TEST_ASSERT(handled_rtp == RtspServer::InterleavedFrameResult::handled_rtp, "rtp channel should be handled");

    const auto malformed_rtp = server.handle_interleaved_frame(0, std::string_view("bad", 3), 9001);
    TEST_ASSERT(malformed_rtp == RtspServer::InterleavedFrameResult::malformed_rtp, "malformed rtp should be flagged");

    const auto unknown_channel = server.handle_interleaved_frame(7, std::string_view("x", 1), 9002);
    TEST_ASSERT(unknown_channel == RtspServer::InterleavedFrameResult::unknown_channel, "unknown channel should be flagged");

    const auto malformed_rtcp = server.handle_interleaved_frame(1, std::string_view("x", 1), 9003);
    TEST_ASSERT(malformed_rtcp == RtspServer::InterleavedFrameResult::malformed_rtcp, "malformed rtcp should be flagged");

    const auto unknown_channel_after_parse = server.handle_interleaved_frame(200, std::string_view("abcd", 4), 9050);
    TEST_ASSERT(unknown_channel_after_parse == RtspServer::InterleavedFrameResult::unknown_channel,
                "unknown channel should remain unknown");

    ::yuan::net::rtcp::RtcpPacket sr;
    sr.kind = ::yuan::net::rtcp::RtcpPacket::Kind::sender_report;
    sr.sender_report.ssrc = 777;
    sr.sender_report.ntp_timestamp = 0x123456789ULL;
    sr.sender_report.rtp_timestamp = 4567;
    sr.sender_report.packet_count = 11;
    sr.sender_report.octet_count = 512;
    ::yuan::buffer::ByteBuffer sr_buf;
    TEST_ASSERT(sr.serialize(sr_buf), "rtcp sender report should serialize");

    const auto handled_rtcp = server.handle_interleaved_frame(
        1,
        std::string_view(sr_buf.read_ptr(), sr_buf.readable_bytes()),
        9004);
    TEST_ASSERT(handled_rtcp == RtspServer::InterleavedFrameResult::handled_rtcp, "rtcp channel should be handled");

    RtspRequest setup_expired_req;
    if (!parse_request_or_fail(
            "SETUP rtsp://example/live/track9 RTSP/1.0\r\n"
            "CSeq: 261\r\n"
            "Session: sid-expire-if;timeout=1\r\n"
            "Transport: RTP/AVP/TCP;unicast;interleaved=20-21\r\n"
            "\r\n",
            setup_expired_req)) {
        return false;
    }
    const RtspResponse setup_expired_resp = server.handle_request(setup_expired_req);
    TEST_ASSERT(setup_expired_resp.status == RtspStatusCode::ok, "setup for expire case should succeed");

    const auto expired_res = server.handle_interleaved_frame(20, std::string_view("x", 1), static_cast<uint64_t>(-1));
    TEST_ASSERT(expired_res == RtspServer::InterleavedFrameResult::session_expired,
                "interleaved frame on recently expired session should report expired");

    const auto unknown_after_expire = server.handle_interleaved_frame(21, std::string_view("x", 1), static_cast<uint64_t>(-1));
    TEST_ASSERT(unknown_after_expire == RtspServer::InterleavedFrameResult::unknown_channel,
                "subsequent unknown frame should be classified as unknown");

    const InterleavedStatsSnapshot stats = server.interleaved_stats_snapshot();
    TEST_ASSERT(stats.handled_rtp >= 1, "handled rtp counter should increase");
    TEST_ASSERT(stats.handled_rtcp >= 1, "handled rtcp counter should increase");
    TEST_ASSERT(stats.malformed_rtp >= 1, "malformed rtp counter should increase");
    TEST_ASSERT(stats.malformed_rtcp >= 1, "malformed rtcp counter should increase");
    TEST_ASSERT(stats.unknown_channel >= 2, "unknown channel counter should increase");
    TEST_ASSERT(stats.session_expired >= 1, "session expired counter should increase");
    return true;
}

bool test_rtsp_server_play_pause_record_interleaved_sequence()
{
    RtspServer server;

    RtspRequest setup_req;
    if (!parse_request_or_fail(
            "SETUP rtsp://example/live/track1 RTSP/1.0\r\n"
            "CSeq: 270\r\n"
            "Session: sid-flow\r\n"
            "Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n"
            "\r\n",
            setup_req)) {
        return false;
    }
    const RtspResponse setup_resp = server.handle_request(setup_req);
    TEST_ASSERT(setup_resp.status == RtspStatusCode::ok, "setup should succeed");

    RtspRequest announce_req;
    if (!parse_request_or_fail(
            "ANNOUNCE rtsp://example/live RTSP/1.0\r\n"
            "CSeq: 271\r\n"
            "Session: sid-flow\r\n"
            "\r\n"
            "v=0\r\n"
            "o=- 1 1 IN IP4 127.0.0.1\r\n"
            "s=demo\r\n"
            "t=0 0\r\n"
            "m=video 0 RTP/AVP 96\r\n"
            "a=rtpmap:96 H264/90000\r\n",
            announce_req)) {
        return false;
    }
    const RtspResponse announce_resp = server.handle_request(announce_req);
    TEST_ASSERT(announce_resp.status == RtspStatusCode::ok, "announce should succeed");

    RtspRequest record_req;
    if (!parse_request_or_fail(
            "RECORD rtsp://example/live RTSP/1.0\r\n"
            "CSeq: 272\r\n"
            "Session: sid-flow\r\n"
            "\r\n",
            record_req)) {
        return false;
    }
    const RtspResponse record_resp = server.handle_request(record_req);
    TEST_ASSERT(record_resp.status == RtspStatusCode::ok, "record should succeed");

    TEST_ASSERT(server.outbound_packet_count() >= 1, "record should enqueue initial feedback packet");
    const auto outbound_after_record = server.drain_outbound_packets(16);
    TEST_ASSERT(!outbound_after_record.empty(), "outbound queue should be drainable after record");
    bool found_rtcp = false;
    for (const auto &pkt : outbound_after_record) {
        if (pkt.is_rtcp) {
            found_rtcp = true;
            break;
        }
    }
    TEST_ASSERT(found_rtcp, "record should enqueue rtcp packet");

    ::yuan::net::rtc::RtcPacket rtp;
    rtp.ssrc = 888;
    rtp.sequence_number = 1;
    rtp.timestamp = 100;
    rtp.payload_type = 96;
    rtp.payload = {0x01, 0x02};
    ::yuan::buffer::ByteBuffer rtp_buf;
    TEST_ASSERT(rtp.serialize(rtp_buf), "rtp should serialize");
    const auto interleaved_during_record =
        server.handle_interleaved_frame(0, std::string_view(rtp_buf.read_ptr(), rtp_buf.readable_bytes()), 0);
    TEST_ASSERT(interleaved_during_record == RtspServer::InterleavedFrameResult::handled_rtp,
                "interleaved rtp should be handled during record");
    TEST_ASSERT(server.outbound_packet_count() >= 1,
                "interleaved media should enqueue automatic rr feedback");

    RtspRequest pause_req;
    if (!parse_request_or_fail(
            "PAUSE rtsp://example/live RTSP/1.0\r\n"
            "CSeq: 273\r\n"
            "Session: sid-flow\r\n"
            "\r\n",
            pause_req)) {
        return false;
    }
    const RtspResponse pause_resp = server.handle_request(pause_req);
    TEST_ASSERT(pause_resp.status == RtspStatusCode::method_not_valid_in_this_state,
                "pause should fail from recording state");
    return true;
}

bool test_rtsp_server_multi_track_setup_rtp_info()
{
    RtspServer server;

    RtspRequest setup_track1;
    if (!parse_request_or_fail(
            "SETUP rtsp://example/live/track1 RTSP/1.0\r\n"
            "CSeq: 30\r\n"
            "Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n"
            "\r\n",
            setup_track1)) {
        return false;
    }
    const RtspResponse setup_resp1 = server.handle_request(setup_track1);
    TEST_ASSERT(setup_resp1.status == RtspStatusCode::ok, "track1 setup should succeed");
    const std::string sid = setup_resp1.headers.at("Session").substr(0, setup_resp1.headers.at("Session").find(';'));

    RtspRequest setup_track2;
    if (!parse_request_or_fail(
            "SETUP rtsp://example/live/track2 RTSP/1.0\r\n"
            "CSeq: 31\r\n"
            "Session: " + sid + "\r\n"
            "Transport: RTP/AVP/TCP;unicast;interleaved=2-3\r\n"
            "\r\n",
            setup_track2)) {
        return false;
    }
    const RtspResponse setup_resp2 = server.handle_request(setup_track2);
    TEST_ASSERT(setup_resp2.status == RtspStatusCode::ok, "track2 setup should succeed");

    RtspRequest play_req;
    if (!parse_request_or_fail(
            "PLAY rtsp://example/live RTSP/1.0\r\n"
            "CSeq: 32\r\n"
            "Session: " + sid + "\r\n"
            "\r\n",
            play_req)) {
        return false;
    }
    const RtspResponse play_resp = server.handle_request(play_req);
    TEST_ASSERT(play_resp.status == RtspStatusCode::ok, "play should succeed");
    const auto it = play_resp.headers.find("RTP-Info");
    TEST_ASSERT(it != play_resp.headers.end(), "rtp-info should exist");
    TEST_ASSERT(it->second.find("track1") != std::string::npos, "rtp-info should include track1");
    TEST_ASSERT(it->second.find("track2") != std::string::npos, "rtp-info should include track2");
    TEST_ASSERT(it->second.find(',') != std::string::npos, "multi-track rtp-info should contain comma");
    return true;
}

bool test_rtsp_server_interleaved_channel_conflict()
{
    RtspServer server;

    RtspRequest setup_track1;
    if (!parse_request_or_fail(
            "SETUP rtsp://example/live/track1 RTSP/1.0\r\n"
            "CSeq: 40\r\n"
            "Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n"
            "\r\n",
            setup_track1)) {
        return false;
    }
    const RtspResponse setup_resp1 = server.handle_request(setup_track1);
    TEST_ASSERT(setup_resp1.status == RtspStatusCode::ok, "first setup should succeed");
    const std::string sid = setup_resp1.headers.at("Session").substr(0, setup_resp1.headers.at("Session").find(';'));

    RtspRequest setup_track2_conflict;
    if (!parse_request_or_fail(
            "SETUP rtsp://example/live/track2 RTSP/1.0\r\n"
            "CSeq: 41\r\n"
            "Session: " + sid + "\r\n"
            "Transport: RTP/AVP/TCP;unicast;interleaved=1-2\r\n"
            "\r\n",
            setup_track2_conflict)) {
        return false;
    }
    const RtspResponse setup_conflict_resp = server.handle_request(setup_track2_conflict);
    TEST_ASSERT(setup_conflict_resp.status == RtspStatusCode::unsupported_transport, "conflicted channels should return 461");
    return true;
}

bool test_rtsp_server_udp_client_port_validation()
{
    RtspServer server;

    RtspRequest bad_udp_setup;
    if (!parse_request_or_fail(
            "SETUP rtsp://example/live/track1 RTSP/1.0\r\n"
            "CSeq: 70\r\n"
            "Transport: RTP/AVP;unicast;client_port=4001-4002\r\n"
            "\r\n",
            bad_udp_setup)) {
        return false;
    }
    const RtspResponse bad_udp_resp = server.handle_request(bad_udp_setup);
    TEST_ASSERT(bad_udp_resp.status == RtspStatusCode::parameter_not_understood, "odd RTP client port should return 451");

    RtspRequest bad_udp_pair_setup;
    if (!parse_request_or_fail(
            "SETUP rtsp://example/live/track1 RTSP/1.0\r\n"
            "CSeq: 71\r\n"
            "Transport: RTP/AVP;unicast;client_port=4000-4004\r\n"
            "\r\n",
            bad_udp_pair_setup)) {
        return false;
    }
    const RtspResponse bad_udp_pair_resp = server.handle_request(bad_udp_pair_setup);
    TEST_ASSERT(bad_udp_pair_resp.status == RtspStatusCode::parameter_not_understood, "non-consecutive client port pair should return 451");

    RtspRequest ok_udp_setup;
    if (!parse_request_or_fail(
            "SETUP rtsp://example/live/track9 RTSP/1.0\r\n"
            "CSeq: 72\r\n"
            "Session: sid-udp-feedback\r\n"
            "Transport: RTP/AVP;unicast;client_port=5000-5001\r\n"
            "\r\n",
            ok_udp_setup)) {
        return false;
    }
    const RtspResponse ok_udp_setup_resp = server.handle_request(ok_udp_setup);
    TEST_ASSERT(ok_udp_setup_resp.status == RtspStatusCode::ok, "valid udp setup should succeed");

    RtspRequest play_udp;
    if (!parse_request_or_fail(
            "PLAY rtsp://example/live RTSP/1.0\r\n"
            "CSeq: 73\r\n"
            "Session: sid-udp-feedback\r\n"
            "\r\n",
            play_udp)) {
        return false;
    }
    const RtspResponse play_udp_resp = server.handle_request(play_udp);
    TEST_ASSERT(play_udp_resp.status == RtspStatusCode::ok, "play on udp session should succeed");

    const auto outbound = server.drain_outbound_packets(16);
    bool found_udp_rtcp = false;
    for (const auto &pkt : outbound) {
        if (pkt.transport == RtspOutboundTransport::udp_unicast && pkt.is_rtcp &&
            pkt.session_id == "sid-udp-feedback" && pkt.udp_remote_port == 5001) {
            found_udp_rtcp = true;
            break;
        }
    }
    TEST_ASSERT(found_udp_rtcp, "udp play should enqueue rtcp feedback to client rtcp port");

    RtspRequest set_short_timeout;
    if (!parse_request_or_fail(
            "SET_PARAMETER rtsp://example/live RTSP/1.0\r\n"
            "CSeq: 74\r\n"
            "Session: sid-udp-feedback\r\n"
            "Content-Type: text/parameters\r\n"
            "\r\n"
            "timeout=1\r\n",
            set_short_timeout)) {
        return false;
    }
    const RtspResponse set_short_timeout_resp = server.handle_request(set_short_timeout);
    TEST_ASSERT(set_short_timeout_resp.status == RtspStatusCode::ok, "set short timeout should succeed");

    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    RtspRequest trigger_expire;
    if (!parse_request_or_fail(
            "OPTIONS rtsp://example/live RTSP/1.0\r\n"
            "CSeq: 75\r\n"
            "\r\n",
            trigger_expire)) {
        return false;
    }
    const RtspResponse trigger_expire_resp = server.handle_request(trigger_expire);
    TEST_ASSERT(trigger_expire_resp.status == RtspStatusCode::ok, "options after timeout window should succeed");
    TEST_ASSERT(server.outbound_packet_count() == 0, "expired udp session should not keep outbound packets");
    return true;
}

bool test_rtsp_server_protocol_edge_cases_reconnect_timeout_and_transport_mix()
{
    RtspServer server;

    RtspRequest setup_first;
    if (!parse_request_or_fail(
            "SETUP rtsp://example/edge/track1 RTSP/1.0\r\n"
            "CSeq: 1000\r\n"
            "Session: sid-edge\r\n"
            "Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n"
            "\r\n",
            setup_first)) {
        return false;
    }
    const RtspResponse setup_first_resp = server.handle_request(setup_first);
    TEST_ASSERT(setup_first_resp.status == RtspStatusCode::ok, "initial setup should succeed");

    RtspRequest setup_dup_cseq;
    if (!parse_request_or_fail(
            "SETUP rtsp://example/edge/track2 RTSP/1.0\r\n"
            "CSeq: 1000\r\n"
            "Session: sid-edge\r\n"
            "Transport: RTP/AVP/TCP;unicast;interleaved=2-3\r\n"
            "\r\n",
            setup_dup_cseq)) {
        return false;
    }
    const RtspResponse setup_dup_cseq_resp = server.handle_request(setup_dup_cseq);
    TEST_ASSERT(setup_dup_cseq_resp.status == RtspStatusCode::method_not_valid_in_this_state,
                "duplicate cseq on setup path should be rejected as invalid state");

    RtspRequest setup_track2;
    if (!parse_request_or_fail(
            "SETUP rtsp://example/edge/track2 RTSP/1.0\r\n"
            "CSeq: 1001\r\n"
            "Session: sid-edge\r\n"
            "Transport: RTP/AVP/TCP;unicast;interleaved=2-3\r\n"
            "\r\n",
            setup_track2)) {
        return false;
    }
    const RtspResponse setup_track2_resp = server.handle_request(setup_track2);
    TEST_ASSERT(setup_track2_resp.status == RtspStatusCode::ok, "second track setup should succeed");

    RtspRequest play_ok;
    if (!parse_request_or_fail(
            "PLAY rtsp://example/edge RTSP/1.0\r\n"
            "CSeq: 1002\r\n"
            "Session: sid-edge\r\n"
            "\r\n",
            play_ok)) {
        return false;
    }
    const RtspResponse play_ok_resp = server.handle_request(play_ok);
    TEST_ASSERT(play_ok_resp.status == RtspStatusCode::ok, "play should succeed");
    TEST_ASSERT(play_ok_resp.headers.find("RTP-Info") != play_ok_resp.headers.end(), "play should include rtp-info");
    TEST_ASSERT(play_ok_resp.headers.at("RTP-Info").find("track1") != std::string::npos,
                "rtp-info should include first track");
    TEST_ASSERT(play_ok_resp.headers.at("RTP-Info").find("track2") != std::string::npos,
                "rtp-info should include second track");

    RtspRequest setup_udp_mix;
    if (!parse_request_or_fail(
            "SETUP rtsp://example/edge/track3 RTSP/1.0\r\n"
            "CSeq: 1003\r\n"
            "Session: sid-edge\r\n"
            "Transport: RTP/AVP;unicast;client_port=6000-6001\r\n"
            "\r\n",
            setup_udp_mix)) {
        return false;
    }
    const RtspResponse setup_udp_mix_resp = server.handle_request(setup_udp_mix);
    TEST_ASSERT(setup_udp_mix_resp.status == RtspStatusCode::ok, "mixed tcp/udp setup should succeed");

    RtspRequest set_keepalive_timeout;
    if (!parse_request_or_fail(
            "SET_PARAMETER rtsp://example/edge RTSP/1.0\r\n"
            "CSeq: 1004\r\n"
            "Session: sid-edge\r\n"
            "Content-Type: text/parameters\r\n"
            "\r\n"
            "timeout=1\r\n",
            set_keepalive_timeout)) {
        return false;
    }
    const RtspResponse set_keepalive_timeout_resp = server.handle_request(set_keepalive_timeout);
    TEST_ASSERT(set_keepalive_timeout_resp.status == RtspStatusCode::ok, "set_parameter keepalive should succeed");

    std::this_thread::sleep_for(std::chrono::milliseconds(1100));

    RtspRequest play_after_timeout;
    if (!parse_request_or_fail(
            "PLAY rtsp://example/edge RTSP/1.0\r\n"
            "CSeq: 1005\r\n"
            "Session: sid-edge\r\n"
            "\r\n",
            play_after_timeout)) {
        return false;
    }
    const RtspResponse play_after_timeout_resp = server.handle_request(play_after_timeout);
    TEST_ASSERT(play_after_timeout_resp.status == RtspStatusCode::request_timeout,
                "play after keepalive timeout should return 408");

    RtspRequest reconnect_setup;
    if (!parse_request_or_fail(
            "SETUP rtsp://example/edge/track1 RTSP/1.0\r\n"
            "CSeq: 1006\r\n"
            "Session: sid-edge\r\n"
            "Transport: RTP/AVP/TCP;unicast;interleaved=10-11\r\n"
            "\r\n",
            reconnect_setup)) {
        return false;
    }
    const RtspResponse reconnect_setup_resp = server.handle_request(reconnect_setup);
    TEST_ASSERT(reconnect_setup_resp.status == RtspStatusCode::ok,
                "reconnect setup with same session id after timeout should recreate session");
    TEST_ASSERT(reconnect_setup_resp.headers.find("Session") != reconnect_setup_resp.headers.end(),
                "reconnect setup should return session header");
    return true;
}

} // namespace

int main()
{
    std::cout << "=== RTSP Server Test Suite ===\n";
    RUN_TEST(test_rtsp_server_cross_request_session_lifecycle);
    RUN_TEST(test_rtsp_server_invalid_session_and_cseq_paths);
    RUN_TEST(test_rtsp_server_multi_track_setup_rtp_info);
    RUN_TEST(test_rtsp_server_interleaved_channel_conflict);
    RUN_TEST(test_rtsp_server_udp_client_port_validation);
    RUN_TEST(test_rtsp_server_protocol_edge_cases_reconnect_timeout_and_transport_mix);
    RUN_TEST(test_rtsp_server_expired_session_returns_timeout);
    RUN_TEST(test_rtsp_server_transport_candidate_fallback);
    RUN_TEST(test_rtsp_server_transport_candidate_errors);
    RUN_TEST(test_rtsp_server_basic_auth_challenge_and_success);
    RUN_TEST(test_rtsp_server_digest_auth_challenge_and_success);
    RUN_TEST(test_rtsp_server_acl_and_rate_limit);
    RUN_TEST(test_rtsp_server_observability_metrics_and_audit);
    RUN_TEST(test_rtsp_server_flush_udp_outbound_packets_metrics);
    RUN_TEST(test_rtsp_server_udp_failure_bucket_and_audit_detail);
    RUN_TEST(test_rtsp_server_udp_retry_and_drop_metrics);
    RUN_TEST(test_rtsp_server_udp_retry_config_zero_drop_immediate);
    RUN_TEST(test_rtsp_server_media_bridge_inject_and_rr);
    RUN_TEST(test_rtsp_server_rtcp_sender_report_updates_bridge);
    RUN_TEST(test_rtsp_server_interleaved_frame_result_paths);
    RUN_TEST(test_rtsp_server_play_pause_record_interleaved_sequence);

    std::cout << "Result: " << g_pass << "/" << g_run << " passed";
    if (g_fail > 0) {
        std::cout << ", " << g_fail << " failed";
    }
    std::cout << "\n";
    return g_fail == 0 ? 0 : 1;
}
