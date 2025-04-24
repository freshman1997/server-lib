#include "content/types.h"
#include "content_type.h"
#include "net/connection/connection.h"
#include "context.h"
#include "http_client.h"
#include "request.h"
#include "response.h"
#include <fstream>

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
    
    client->connect({"192.168.1.114", 5244}, 
    [](net::http::HttpRequest *req) {
        req->set_raw_url("/x5client-trunk-pc/x5client-latest.zip");
        req->add_header("Connection", "close");
        req->add_header("Host", "192.168.1.114:5244");
        req->send();
    },
    [](net::http::HttpRequest *req, net::http::HttpResponse *resp){
        if (resp->good()) {
            if (resp->get_response_code() == net::http::ResponseCode::ok_) {
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
                }
                resp->get_context()->get_connection()->close();
            } else if (resp->get_response_code() == net::http::ResponseCode::bad_request) {
                std::cerr << "Bad request\n";
            } else if (resp->get_response_code() == net::http::ResponseCode::not_found) {
                std::cerr << "Not found\n";
            } else {
                std::cerr << "Internal server error\n";
            }
        } else {
            std::cerr << "Error: parse packet failed!!!" << '\n';
        }
    });

#ifdef _WIN32
    WSACleanup();
#endif

    return 0;
}