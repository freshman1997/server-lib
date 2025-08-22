#ifndef __NET_HTTP_PACKET_H__
#define __NET_HTTP_PACKET_H__
#include "attachment/attachment.h"
#include "buffer/buffer.h"
#include "buffer/linked_buffer.h"
#include "content_type.h"
#include "content/types.h"
#include "packet_parser.h"
#include "response_code.h"
#include "task/task.h"

#include <memory>
#include <string>
#include <vector>

namespace yuan::net
{
    class Connection;
}

namespace yuan::net::http 
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
    class ContentParser;

    class HttpPacket
    {
    public:
        HttpPacket(HttpSessionContext *context);
        virtual ~HttpPacket();

    public:
        virtual bool pack_header(Connection *conn = nullptr) = 0;

        virtual PacketType get_packet_type() = 0;

        virtual void reset();

    public:
        void send();

        bool parse(buffer::Buffer &buff);

        bool write(buffer::Buffer &buff);

        void add_header(const std::string &k, const std::string &v);

        void remove_header(const std::string &k);

        void clear_header();

        void set_body_length(uint32_t len);

        uint32_t get_body_length()
        {
            return body_length_;
        }

        void set_version(HttpVersion);

        bool is_ok() const;

        HttpVersion get_version() const;

        std::string get_raw_version() const;

        std::unordered_map<std::string, std::vector<std::string>> & get_request_params()
        {
            return params_;
        }

        const std::string * get_header(const std::string &key);

        const char * body_begin();

        const char * body_end();

        ContentType get_content_type() const
        {
            return content_type_;
        }

        const std::unordered_map<std::string, std::string> & get_content_type_extra() const 
        {
            return content_type_extra_;
        }

        void set_body_content(Content *content)
        {
            if (body_content_) {
                delete body_content_;
            }
            body_content_ = content;
        }

        Content * get_body_content()
        {
            return body_content_;
        }

        bool good() const 
        {
            return is_good_ || is_download_file_;
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

        buffer::Buffer * get_buff(bool take = false, bool reset = true);

        void pack_and_send(Connection *conn);

        bool is_chunked() const;

        void set_chunked_checksum(const std::string &checksum)
        {
            chunked_checksum_ = checksum;
        }

        const std::string & get_chunked_checksum() const
        {
            return chunked_checksum_;
        }

        const std::string get_content_charset() const;

        void set_body_state(BodyState state);

        BodyState get_body_state() const
        {
            return parser_->get_body_state();
        }
        
        void set_pre_content_parser(ContentParser *parser)
        {
            pre_content_parser_ = parser;
        }

        ContentParser * get_pre_content_parser()
        {
            return pre_content_parser_;
        }

        void swap_buffer(buffer::Buffer *buf);

    public: // task
        void set_task(HttpTask *task)
        {
            task_ = task;
        }

        HttpTask * get_task()
        {
            return task_;
        }
        
    public: // original file name
        void set_original_file_name(const std::string &name)
        {
            original_file_name_ = name;
        }

        const std::string & get_original_file_name() const
        {
            return original_file_name_;
        }

        bool is_donwloading()
        {
            return is_download_file_;
        }

        bool is_uploading()
        {
            return is_upload_file_ && task_;
        }

        void set_downlload_file(bool flag)
        {
            is_download_file_ = flag;
        }

        void set_upload_file(bool flag)
        {
            is_upload_file_ = flag;
        }

        bool is_task_prepared()
        {
            return task_ && (is_download_file_ ? task_->get_task_type() == HttpTaskType::download_file_ : task_->get_task_type() == HttpTaskType::upload_file_);
        }

        bool is_pending_large_block()
        {
            return is_download_file_ || is_upload_file_;
        }

    protected:
        HttpSessionContext *context_;
        HttpPacketParser *parser_;
        HttpVersion version_ = HttpVersion::v_1_1;
        bool is_good_;
        ResponseCode error_code_;
        uint32_t body_length_;
        ContentType content_type_;
        std::unordered_map<std::string, std::vector<std::string>> params_;
        std::unordered_map<std::string, std::string> headers_;
        std::string content_type_text_;
        std::unordered_map<std::string, std::string> content_type_extra_;
        Content *body_content_;
        buffer::Buffer * buffer_;
        ContentParser *pre_content_parser_;
        std::string chunked_checksum_;
        std::string original_file_name_;
        bool is_upload_file_;
        bool is_download_file_;
        HttpTask *task_;
        buffer::LinkedBuffer linked_buffer_;
    };
}

#endif