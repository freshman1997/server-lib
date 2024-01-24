#ifndef __HTTP_REQUEST_CONTEXT_H__
#define __HTTP_REQUEST_CONTEXT_H__

namespace net 
{
    class TcpConnection;
}

namespace net::http 
{
    class HttpRequest;
    class HttpResponse;

    class HttpRequestContext
    {
    public:
        HttpRequestContext(TcpConnection *conn_);
        ~HttpRequestContext();

        HttpRequest * get_request()
        {
            return request;
        }

        HttpResponse * get_response()
        {
            return response;
        }

        TcpConnection * get_connection()
        {
            return conn_;
        }

    public:
        bool parse();

    private:
        TcpConnection *conn_;
        HttpRequest *request;
        HttpResponse *response;
    };
}

#endif
