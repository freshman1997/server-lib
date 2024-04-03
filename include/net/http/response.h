#ifndef __RESPONSE_H__
#define __RESPONSE_H__
#include <string>
#include <unordered_map>

#include "net/http/packet.h"
#include "response_code.h"

namespace net::http
{
    class HttpSessionContext;
    class HttpResponseParser;
    
    class HttpResponse : public HttpPacket
    {
    public:
        HttpResponse(HttpSessionContext *context);
        ~HttpResponse();

    public:
        virtual void reset();

        virtual bool pack_header();

        virtual PacketType get_packet_type()
        {
            return PacketType::response;
        }

    public:
        void set_response_code(ResponseCode code)
        {
            respCode_ = code;
        }

        void append_body(const char *data);
        
        void append_body(const std::string &data);

    private:
        bool pack_response();

    private:
        ResponseCode respCode_ = ResponseCode::bad_request;
    };
}

#endif
