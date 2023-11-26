#include "buffer.h"
#include "request.h"
#include <iostream>
#include <string>

using namespace std;

int main()
{
    string raw = "get /hello.php http/1.1\r\nContent-Type:audio/mp3 \r\nContent-Length:1024\r\n\r\n";

    HttpRequest req;
    Buffer buff;
    buff.write_string(raw);
    if (!req.parse_header(buff)) {
        std::cout << "parse fail!!" << std::endl;
        return 1;
    }

    return 0;
}