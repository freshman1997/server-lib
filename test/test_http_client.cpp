#include "attachment/attachment.h"
#include "base/time.h"
#include "content/types.h"
#include "content_type.h"
#include "header_key.h"
#include "http_client.h"
#include "request.h"
#include "response.h"
#include "task/download_file_task.h"

#include <fstream>
#include <iostream>
#include <memory>

namespace
{
int handle_response(yuan::net::http::HttpResponse *resp)
{
    using namespace yuan;

    if (!resp) {
        std::cerr << "Request failed\n";
        return 1;
    }

    if (!resp->good()) {
        std::cerr << "Error: parse packet failed!!!\n";
        return 1;
    }

    if (resp->get_response_code() == net::http::ResponseCode::bad_request) {
        std::cerr << "Bad request\n";
        return 1;
    }

    if (resp->get_response_code() == net::http::ResponseCode::not_found) {
        std::cerr << "Not found\n";
        return 1;
    }

    if (resp->get_response_code() != net::http::ResponseCode::ok_) {
        std::cerr << "Internal server error\n";
        return 1;
    }

    if (resp->is_downloading()) {
        std::cout << "File download, filename: " << resp->get_original_file_name()
                  << ", contentType: " << static_cast<int>(resp->get_content_type())
                  << ", length: " << resp->get_body_length() << std::endl;

        auto attachment = std::make_shared<net::http::AttachmentInfo>();
        attachment->tmp_file_name_ = "___tmp___" + std::to_string(base::time::get_tick_count()) + ".tmp";
        attachment->origin_file_name_ = resp->get_original_file_name();

        const auto content_type = resp->get_header(net::http::http_header_key::content_type);
        attachment->content_type_ = content_type ? *content_type : "application/octet-stream";
        attachment->length_ = resp->get_body_length();
        attachment->offset_ = 0;

        auto *task = new net::http::HttpDownloadFileTask([resp]() {
            std::cout << "File download completed: " << resp->get_original_file_name() << std::endl;
            resp->set_download_file(false);
        });

        task->set_attachment_info(attachment);
        if (!task->init()) {
            std::cerr << "Failed to initialize task\n";
            delete task;
            return 1;
        }

        resp->set_task(task);
        resp->dispatch_task();
        return 0;
    }

    const net::http::Content *content = resp->get_body_content();
    if (!content || !content->is_valid()) {
        std::cerr << "No valid body content\n";
        return 1;
    }

    if (content->type == net::http::ContentType::text_html ||
        content->type == net::http::ContentType::text_plain) {
        auto *text_content = content->as<net::http::TextContent>();
        if (text_content && !text_content->data.empty()) {
            std::cout << text_content->data << std::endl;
            return 0;
        }

        auto *chunked_content = content->as<net::http::ChunkedContent>();
        if (!chunked_content || chunked_content->tmp_file.empty()) {
            std::cerr << "No text data available\n";
            return 1;
        }

        std::ifstream file(chunked_content->tmp_file, std::ios::binary);
        if (!file.is_open()) {
            std::cerr << "Failed to open tmp file: " << chunked_content->tmp_file << std::endl;
            return 1;
        }

        std::string data(chunked_content->total_bytes + 1, '\0');
        file.read(&*data.begin(), static_cast<std::streamsize>(chunked_content->total_bytes));
        std::cout << data << std::endl;
        return 0;
    }

    if (content->type == net::http::ContentType::application_json) {
        auto *json_content = content->as<net::http::JsonContent>();
        if (json_content) {
            std::cout << json_content->value.dump(4) << std::endl;
            return 0;
        }
    }

    if (content->type == net::http::ContentType::multpart_form_data) {
        auto *form_data = content->as<net::http::FormDataContent>();
        if (form_data) {
            for (const auto &[name, item] : form_data->fields) {
                std::cout << "Field: " << name << " [type=" << static_cast<int>(item->type) << "]" << std::endl;
            }
            return 0;
        }
    }

    if (content->type == net::http::ContentType::multpart_byte_ranges) {
        auto *range_data = content->as<net::http::RangeDataContent>();
        if (range_data) {
            std::cout << "Range chunks: " << range_data->chunks.size() << std::endl;
            return 0;
        }
    }

    if (content->type == net::http::ContentType::chunked) {
        auto *chunked_content = content->as<net::http::ChunkedContent>();
        if (chunked_content) {
            std::cout << "Chunked transfer complete, total bytes: "
                      << chunked_content->total_bytes
                      << ", tmp_file: " << chunked_content->tmp_file << std::endl;
            return 0;
        }
    }

    return 0;
}
} // namespace

int main()
{
#ifdef _WIN32
    WSADATA wsa;
    if (const int result = WSAStartup(MAKEWORD(2, 2), &wsa); result != NO_ERROR) {
        wprintf(L"WSAStartup failed with error: %d\n", result);
        return 1;
    }
#endif

    using namespace yuan;

    auto client = new net::http::HttpClient;
    if (!client->query("http://192.168.1.71:5244")) {
        std::cerr << "Failed to query\n";
        delete client;
#ifdef _WIN32
        WSACleanup();
#endif
        return 1;
    }

    auto *response = client->connect_async([](net::http::HttpRequest *req) {
        req->set_raw_url("/p/CDN/x5client-trunk-pc/x5client-latest.zip");
        req->add_header("Connection", "close");
        req->add_header("Host", "192.168.1.71:5244");
        req->send();
    }).execute();

    const int exit_code = handle_response(response);
    delete client;

#ifdef _WIN32
    WSACleanup();
#endif

    return exit_code;
}
