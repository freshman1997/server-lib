#include "net/connection/connection.h"
#include "context.h"
#include "http_client.h"
#include "request.h"
#include "response.h"

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

    client->connect({"www.baidu.com", 80}, 
    [](net::http::HttpRequest *req) {
        req->add_header("Connection", "close");
        req->send();
    },
    [](net::http::HttpRequest *req, net::http::HttpResponse *resp){
        if (resp->good()) {
            const net::http::Content *content = resp->get_body_content();
            std::string data(resp->body_begin(), resp->body_end());
            std::cout << data << std::endl;
            resp->get_context()->get_connection()->close();
        }
    });

#ifdef _WIN32
    WSACleanup();
#endif

    return 0;
}