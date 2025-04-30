#include "attachment/attachment.h"
#include "base/time.h"
#include "content/types.h"
#include "content_type.h"
#include "header_key.h"
#include "net/connection/connection.h"
#include "context.h"
#include "http_client.h"
#include "request.h"
#include "response.h"
#include "task/download_file_task.h"
#include <fstream>
#include <iostream>
#include <memory>

int main()
{
#ifdef _WIN32
    WSADATA wsa;
    if (const int iResult = WSAStartup(MAKEWORD(2, 2), &wsa);iResult != NO_ERROR) {
        wprintf(L"WSAStartup failed with error: %d\n", iResult);
        return 1;
    }
#endif

    using namespace yuan;
    net::http::HttpClient *client = new net::http::HttpClient;
    client->connect({"192.168.1.71", 5244}, 
    [](net::http::HttpRequest *req) {
        req->set_raw_url("/p/CDN/x5client-trunk-pc/x5client-latest.zip");
        req->add_header("Connection", "close");
        req->add_header("Host", "192.168.1.71:5244");
        req->send();
    },
    [](net::http::HttpRequest *req, net::http::HttpResponse *resp){
        if (resp->good()) {
            if (resp->get_response_code() == net::http::ResponseCode::ok_) {
                if (resp->is_process_large_block()) {
                    std::cout << "File download, filename: " << resp->get_original_file_name() << "contextType: << " << (int)resp->get_content_type() << ", length: " << resp->get_body_length() << std::endl;

                    auto attachment = std::make_shared<yuan::net::http::AttachmentInfo>();
                    attachment->tmp_file_name_ = "___tmp___" + std::to_string(yuan::base::time::get_tick_count()) + ".tmp";
                    attachment->origin_file_name_ = resp->get_original_file_name();

                    auto contentType = resp->get_header(yuan::net::http::http_header_key::content_type);
                    attachment->content_type_ = contentType ? *contentType : "application/octet-stream";
                    attachment->length_ = resp->get_body_length();
                    attachment->offset_ = 0;
                    
                    resp->get_context()->set_is_pro_large_block(true);

                    net::http::HttpDownloadFileTask *task = new net::http::HttpDownloadFileTask([resp]() {
                        std::cout << "File download completed: " << resp->get_original_file_name() << std::endl;
                        resp->set_process_large_block(false);
                        resp->get_context()->set_is_pro_large_block(false);
                    });

                    task->set_attachment_info(attachment);

                    if (!task->init()) {
                        std::cerr << "Failed to initialize task" << std::endl;
                        delete task;
                        resp->get_context()->get_connection()->close();
                        return;
                    }

                    resp->set_task(task);
                    resp->dispatch_task();

                    return;
                }

                const net::http::Content *content = resp->get_body_content();
                if (content->type_ == yuan::net::http::ContentType::text_html) {
                    if (content->content_data_ == nullptr) {
                        std::ifstream file(content->file_info_.tmp_file_name_, std::ios::binary);
                        if (!file.is_open()) {
                            std::cerr << "Failed to open file: " << content->file_info_.tmp_file_name_ << std::endl;
                            return;
                        }

                        std::string data(content->file_info_.file_size_ + 1, '\0');
                        file.read(&*data.begin(), content->file_info_.file_size_);
                        std::cout << data << std::endl;
                        file.close();
                    } else {
                        const net::http::TextContent *textContent = static_cast<const net::http::TextContent *>(content->content_data_);
                        std::cout << textContent->get_content() << std::endl;
                    }
                } else if (content->type_ == yuan::net::http::ContentType::application_json) {
                    const net::http::JsonContent *jsonContent = static_cast<const net::http::JsonContent *>(content->content_data_);
                    std::cout << jsonContent->jval.dump(4) << std::endl;
                } else if (content->type_ == yuan::net::http::ContentType::multpart_form_data) {
                    // Handle multipart form data
                } else if (content->type_ == yuan::net::http::ContentType::multpart_byte_ranges) {
                    // Handle multipart byte ranges
                }
            } else if (resp->get_response_code() == net::http::ResponseCode::bad_request) {
                std::cerr << "Bad request\n";
            } else if (resp->get_response_code() == net::http::ResponseCode::not_found) {
                std::cerr << "Not found\n";
            } else {
                std::cerr << "Internal server error\n";
            }
            resp->get_context()->get_connection()->close();
        } else {
            std::cerr << "Error: parse packet failed!!!" << '\n';
        }
    });

#ifdef _WIN32
    WSACleanup();
#endif

    return 0;
}