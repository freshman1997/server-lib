#include "buffer/byte_buffer.h"
#include "context.h"
#include "request.h"
#include "response_code.h"
#include "webdav.h"

#include <filesystem>
#include <fstream>
#include <iostream>

namespace
{
    int failed = 0;

    void check(bool cond, const char *msg)
    {
        if (!cond) {
            ++failed;
            std::cerr << "[FAIL] " << msg << '\n';
        }
    }

    void test_method_parser()
    {
        yuan::net::http::HttpSessionContext ctx(nullptr);
        yuan::buffer::ByteBuffer req(std::string_view(
            "PROPFIND /dav/a.txt HTTP/1.1\r\n"
            "Host: local\r\n"
            "Depth: 1\r\n"
            "\r\n"));
        check(ctx.parse_from(req), "PROPFIND request should parse");
        check(ctx.get_request()->get_method() == yuan::net::http::HttpMethod::propfind_,
              "method should be propfind");
        check(ctx.get_request()->get_path() == "/dav/a.txt", "path should parse");
    }

    void test_local_backend_crud()
    {
        const auto root = std::filesystem::temp_directory_path() / "yuan_webdav_test_backend";
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
        std::filesystem::create_directories(root, ec);

        yuan::net::webdav::LocalWebDavBackend fs(root);
        auto mk = fs.make_collection("/docs");
        check(mk.ok && mk.status == 201, "MKCOL should create collection");

        auto put = fs.write("/docs/readme.txt", "hello dav", true);
        check(put.ok && put.status == 201, "PUT should create file");
        auto info = fs.stat("/docs/readme.txt");
        check(info.exists && !info.is_collection && info.content_length == 9, "stat should expose file metadata");

        auto body = fs.read("/docs/readme.txt");
        check(body && *body == "hello dav", "read should return file body");

        auto cp = fs.copy("/docs/readme.txt", "/docs/copy.txt", false);
        check(cp.ok && cp.status == 201, "COPY should create destination");
        auto mv = fs.move("/docs/copy.txt", "/docs/moved.txt", true);
        check(mv.ok && mv.status == 201, "MOVE should create new destination");
        check(!fs.stat("/docs/copy.txt").exists && fs.stat("/docs/moved.txt").exists,
              "MOVE should remove source and keep destination");

        auto patch = fs.set_properties("/docs/moved.txt", { { "DAV:", "nas-tag", "photos" } });
        check(patch.ok, "PROPPATCH property store should persist");
        auto props = fs.dead_properties("/docs/moved.txt");
        check(!props.empty() && props.front().name == "nas-tag", "dead property should be readable");

        auto rm = fs.remove("/docs");
        check(rm.ok && rm.status == 204, "DELETE should remove recursively");
        std::filesystem::remove_all(root, ec);
    }

    void test_locks_and_xml()
    {
        yuan::net::webdav::WebDavLockManager locks;
        auto lock = locks.create("/share", yuan::net::webdav::LockScope::exclusive,
                                 yuan::net::webdav::Depth::infinity, "owner",
                                 std::chrono::seconds(60));
        check(!locks.allows("/share/file.txt", ""), "exclusive depth-infinity lock should cover child");
        check(locks.allows("/share/file.txt", "<" + lock.token + ">"), "matching token should allow write");
        check(locks.unlock("<" + lock.token + ">"), "UNLOCK should remove lock");
        check(locks.allows("/share/file.txt", ""), "unlocked resource should allow write");

        yuan::net::webdav::ResourceInfo info;
        info.exists = true;
        info.display_name = "file & one.txt";
        info.content_length = 5;
        info.last_modified = std::chrono::system_clock::now();
        info.etag = "\"5-1\"";
        const auto xml = yuan::net::webdav::propfind_response_xml(
            "/file.txt", info, {}, {}, {});
        check(xml.find("file &amp; one.txt") != std::string::npos, "XML values should be escaped");
        check(xml.find("getcontentlength") != std::string::npos, "PROPFIND XML should include live properties");
    }
}

int main()
{
    test_method_parser();
    test_local_backend_crud();
    test_locks_and_xml();

    if (failed != 0) {
        std::cerr << "webdav tests failed=" << failed << '\n';
        return 1;
    }
    std::cout << "webdav tests passed\n";
    return 0;
}
