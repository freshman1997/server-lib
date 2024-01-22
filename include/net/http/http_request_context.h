#ifndef __HTTP_CONTEXT_H__
#define __HTTP_CONTEXT_H__

namespace net::http 
{
    class HttpRequest;
    class HttpResponse;

    class HttpRequestContext
    {
    public:
        HttpRequestContext();
        ~HttpRequestContext();

        

    private:
        HttpRequest *request;
        HttpResponse *response;
    };
}

#endif
