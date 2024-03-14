#ifndef __HTTP_REQUEST_CONTEXT_H__
#define __HTTP_REQUEST_CONTEXT_H__

namespace net 
{
    class Connection;
}

namespace net::http 
{
    class HttpRequest;
    class HttpResponse;

    class HttpRequestContext
    {
    public:
        HttpRequestContext(Connection *conn_);
        ~HttpRequestContext();

        HttpRequest * get_request()
        {
            return request_;
        }

        HttpResponse * get_response()
        {
            return response_;
        }

        Connection * get_connection()
        {
            return conn_;
        }

        void send();

    public:
        bool parse();

    private:
        Connection *conn_;
        HttpRequest *request_;
        HttpResponse *response_;
    };
}

#endif
