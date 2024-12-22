#include "response_code.h"
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ios>
#include <iostream>
#include <string>
#include <fstream>
#include <ctime>
#include <cstdlib>

#ifndef _WIN32
#include <signal.h>
#else
#include <winsock2.h>
#include <WS2tcpip.h>
#include <stdio.h>
#include <windows.h>
#endif

#include "buffer/buffer.h"
#include "header_key.h"
#include "context.h"
#include "http_server.h"
#include "request.h"
#include "response.h"
#include "net/connection/connection.h"

using namespace yuan;

class VideoTest
{
public:
    VideoTest()
    {
        content_size_ = -1;
        file_.open("e:/1.mp4");
        if (!file_.good()) {
            std::cout << "open file fail!\n";
            return;
        }

        file_.seekg(0, std::ios_base::end);
        length_ = file_.tellg();
        if (length_ == 0) {
            file_.close();
            std::cout << "open file fail1!\n";
            return;
        }

        content_size_ = 1024 * 1024;
    }

    void on_request(net::http::HttpRequest *req, net::http::HttpResponse *resp)
    {
        if (content_size_ < 0) {
            resp->get_context()->process_error();
            return;
        }

        const std::string *range = req->get_header(net::http::http_header_key::range);
        long long offset = 0;

        if (range) {
            size_t pos = range->find_first_of("=");
            if (std::string::npos == pos) {
                resp->get_context()->process_error();
                return;
            }

            size_t pos1 = range->find_first_of("-");
            offset = std::atol(range->substr(pos + 1, pos1 - pos).c_str());
        }

        std::string bytes = "bytes ";
        bytes.append(std::to_string(offset))
            .append("-")
            .append(std::to_string(length_ - 1))
            .append("/")
            .append(std::to_string(length_));
        
        resp->add_header("Content-Type", "video/mp4");
        resp->add_header("Content-Range", bytes);

        size_t r = content_size_ + offset > length_ ? length_ - offset : content_size_;
        //std::cout << "offset: " << offset << ", length: " << r << ", size: " << length_ << std::endl;

        resp->add_header("Content-length", std::to_string(r));

        file_.seekg(offset, std::ios::beg);
        resp->get_buff()->reset();
        if (resp->get_buff()->writable_size() < content_size_) {
            resp->get_buff()->resize(content_size_);
        }

        file_.read(resp->get_buff()->buffer_begin(), content_size_);
        resp->get_buff()->fill(r);

        if (file_.eof()) {
            file_.clear();
        }

        resp->add_header("Accept-Ranges", "bytes");
        resp->set_response_code(net::http::ResponseCode::partial_content);
        resp->send();
    }

    void on_body_test(net::http::HttpRequest *req, net::http::HttpResponse *resp)
    {
        if (req->body_begin()) {
            std::cout << "has body\n";
        }

        /*using namespace nlohmann;
        json jData = json::parse(req->body_begin(), req->body_end());
        */
        //std::string jData(req->body_begin(), req->body_end());
        //req->read_body_done();

        //std::cout << jData << std::endl;

        std::string body = "{\"success\": 1}";
        resp->add_header("Content-length", std::to_string(body.size()));
        resp->add_header("Connection", "close");
        resp->add_header("Content-Type", "application/json");
        resp->set_response_code(net::http::ResponseCode::ok_);

        resp->append_body(body);
        resp->send();
    }

    void icon(net::http::HttpRequest *req, net::http::HttpResponse *resp)
    {
        resp->add_header("Connection", "close");
        resp->add_header("Content-Type", "image/x-icon");
        resp->set_response_code(net::http::ResponseCode::ok_);
        std::fstream file;
        file.open("/home/yuan/Desktop/icon.ico");
        if (!file.good()) {
            resp->get_context()->process_error(net::http::ResponseCode::not_found);
            return;
        }

        file.seekg(0, std::ios_base::end);
        std::size_t sz = file.tellg();
        resp->get_buff()->resize(sz);
        file.seekg(0, std::ios_base::beg);
        file.read(resp->get_buff()->buffer_begin(), sz);
        resp->get_buff()->fill(sz);

        resp->add_header("Content-length", std::to_string(sz));
        resp->send();
    }

    void serve_static(net::http::HttpRequest *req, net::http::HttpResponse *resp)
    {

    }

private:
    int content_size_;
    std::fstream file_;
    long long length_;
};

int main()
{
#ifdef _WIN32
    WSADATA wsa;
    if (const int iResult = WSAStartup(MAKEWORD(2, 2), &wsa);iResult != NO_ERROR) {
        wprintf(L"WSAStartup failed with error: %d\n", iResult);
        return 1;
    }
#endif

    net::http::HttpServer server;
    VideoTest vt;

    server.on("/movie", [&vt](net::http::HttpRequest *req, net::http::HttpResponse *resp) {
        vt.on_request(req, resp);
    });

    server.on("/body", [&vt](net::http::HttpRequest *req, net::http::HttpResponse *resp) {
        vt.on_body_test(req, resp);
    });

    server.on("/favicon.ico", [&vt](net::http::HttpRequest *req, net::http::HttpResponse *resp) {
        vt.icon(req, resp);
    });

    if (!server.init(12333)) {
        std::cout << " init failed " << std::endl;
        return 1;
    }

    server.serve();
    
#ifdef _WIN32
    WSACleanup();
#endif
}