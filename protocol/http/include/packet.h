#ifndef __NET_HTTP_PACKET_H__
#define __NET_HTTP_PACKET_H__
#include "buffer/buffer.h"
#include "content_type.h"
#include "content/types.h"
#include "response_code.h"

#include <string>

namespace net::http 
{
    enum class HttpVersion : char
    {
        invalid = -1,
        v_1_0,
        v_1_1,
        v_2_0,
        v_3_0,
    };

    enum class PacketType
    {
        request,
        response
    };

    class HttpSessionContext;
    class HttpPacketParser;

    class HttpPacket
    {
    public:
        HttpPacket(HttpSessionContext *context);
        virtual ~HttpPacket();

    public:
        virtual bool pack_header() = 0;

        virtual PacketType get_packet_type() = 0;

        virtual void reset();

    public:
        void send();

        bool parse(Buffer &buff);

        void add_header(const std::string &k, const std::string &v);

        void set_body_length(uint32_t len);

        uint32_t get_body_length()
        {
            return body_length_;
        }

        void set_version(HttpVersion);

        bool is_ok() const;

        HttpVersion get_version() const;

        std::string get_raw_version() const;

        std::unordered_map<std::string, std::string> & get_request_params()
        {
            return params_;
        }

        const std::string * get_header(const std::string &key);

        const char * body_begin();

        const char * body_end();

        void read_body_done();

        ContentType get_content_type() const
        {
            return content_type_;
        }

        const std::unordered_map<std::string, std::string> & get_content_type_extra() const 
        {
            return content_type_extra_;
        }

        void set_body_content(const Content *content)
        {
            body_content_ = content;
        }

        const Content * get_body_content() const
        {
            return body_content_;
        }

        bool good() const 
        {
            return is_good_;
        }

        ResponseCode get_error_code() const
        {
            return error_code_;
        }

        void set_error_code(ResponseCode code)
        {
            error_code_ = code;
        }

        HttpSessionContext * get_context()
        {
            return context_;
        }

        std::pair<bool, uint32_t> parse_content_type(const char *begin, const char *end, std::string &type, std::unordered_map<std::string, std::string> &extra);

        bool parse_content();

        Buffer * get_buff(bool take = false);

    protected:
        HttpSessionContext *context_;
        HttpPacketParser *parser_;
        HttpVersion version_ = HttpVersion::v_1_1;
        bool is_good_;
        ResponseCode error_code_;
        uint32_t body_length_;
        ContentType content_type_;
        std::unordered_map<std::string, std::string> params_;
        std::unordered_map<std::string, std::string> headers_;
        std::string content_type_text_;
        std::unordered_map<std::string, std::string> content_type_extra_;
        const Content *body_content_;
        Buffer * buffer_;
    };
}

#endif