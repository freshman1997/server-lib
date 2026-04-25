#ifndef __NET_HTTP_PACKET_H__
#define __NET_HTTP_PACKET_H__
#include "buffer/byte_buffer.h"
#include "content_type.h"
#include "content/types.h"
#include "packet_parser.h"
#include "response_code.h"
#include "task/task.h"

#include <string>
#include <string_view>
#include <vector>
#include <unordered_map>
#include <memory>
#include <filesystem>
#include <fstream>

namespace yuan::net
{
    class Connection;
}

namespace yuan::net::http
{
    enum class HttpVersion : char {
        invalid = -1,
        v_1_0,
        v_1_1,
        v_2_0,
        v_3_0,
    };

    enum class PacketType {
        request,
        response
    };

    class HttpSessionContext;
    class HttpPacketParser;
    class ContentParser;

    class HttpPacket
    {
    private:
        template <typename T>
        static T *ptr_of(const std::unique_ptr<T> &owner)
        {
            return owner ? const_cast<T *>(&*owner) : nullptr;
        }

        template <typename T>
        static T *ptr_of(const std::shared_ptr<T> &owner)
        {
            return owner ? const_cast<T *>(&*owner) : nullptr;
        }

    public:
        HttpPacket(HttpSessionContext *context);
        virtual ~HttpPacket();

        HttpPacket(const HttpPacket &) = delete;
        HttpPacket &operator=(const HttpPacket &) = delete;
        HttpPacket(HttpPacket &&other) noexcept;
        HttpPacket &operator=(HttpPacket &&other) noexcept;

    public:
        virtual bool pack_header(Connection *conn = nullptr) = 0;

        virtual PacketType get_packet_type() = 0;

        virtual void reset();

    public:
        void send();

        bool parse(const ::yuan::buffer::ByteBuffer &buff);

        bool write(::yuan::buffer::ByteBuffer &buff);

        // 原始string版本（兼容）
        void add_header(const std::string &k, const std::string &v);
        // move语义版本
        void add_header(std::string &&k, std::string &&v);
        // const char* 避免临时string构造
        void add_header(const char *k, const char *v);
        // string_view 查询，避免构造string key
        const std::string *get_header(std::string_view key) const;

        void remove_header(std::string_view k);

        void clear_header();

        void set_body_length(uint32_t len);

        uint32_t get_body_length() const
        {
            return body_length_;
        }

        void set_version(HttpVersion);

        bool is_ok() const;

        HttpVersion get_version() const;

        std::string get_raw_version() const;

        std::unordered_map<std::string, std::vector<std::string> > &get_request_params()
        {
            return params_;
        }

        ContentType get_content_type() const
        {
            return content_type_;
        }

        const std::unordered_map<std::string, std::string> &get_content_type_extra() const
        {
            return content_type_extra_;
        }

        // 使用shared_ptr管理body_content生命周期
        void set_body_content(Content *content)
        {
            body_content_.reset(content);
        }

        Content *get_body_content() const
        {
            return ptr_of(body_content_);
        }

        bool good() const
        {
            return is_good_ || is_download_file_;
        }

        ResponseCode get_error_code() const
        {
            return error_code_;
        }

        void set_error_code(const ResponseCode code)
        {
            error_code_ = code;
        }

        HttpSessionContext *get_context() const
        {
            return context_;
        }

        std::pair<bool, uint32_t> parse_content_type(const char *begin, const char *end, std::string &type, std::unordered_map<std::string, std::string> &extra);

        bool parse_content();

        void reserve_body_buffer(std::size_t size);

        char *body_write_ptr();

        void commit_body_bytes(std::size_t size);

        void append_body(std::string_view text);

        std::size_t body_buffer_size() const;
        std::string body_buffer_text() const;
        const char *body_buffer_begin() const;
        const char *body_buffer_end() const;
        ::yuan::buffer::ByteBuffer take_body_output_buffer();

        void set_body_file_path(std::filesystem::path path);
        const std::filesystem::path &body_file_path() const;
        bool has_body_file() const;
        bool begin_body_file_spool(std::uint32_t expected_length);
        bool append_body_file_bytes(const char *data, std::size_t size);
        bool body_file_spool_done() const;
        std::size_t body_file_received() const;

        void pack_and_send(Connection *conn);

        bool is_chunked() const;

        void set_chunked_checksum(std::string checksum)
        {
            chunked_checksum_ = std::move(checksum);
        }

        const std::string &get_chunked_checksum() const
        {
            return chunked_checksum_;
        }

        std::string get_content_charset() const;

        void set_body_state(BodyState state);

        BodyState get_body_state() const
        {
            return parser_->get_body_state();
        }

        // body数据指针访问（零拷贝）
        const char *body_begin();
        const char *body_end();

        ::yuan::buffer::ByteBuffer take_body_buffer();
        ::yuan::buffer::ByteBuffer take_leftover_buffer();
        void replace_body_buffer(::yuan::buffer::ByteBuffer buffer);

        void set_pre_content_parser(ContentParser *parser);

        void set_pre_content_parser(std::shared_ptr<ContentParser> parser)
        {
            pre_content_parser_ = std::move(parser);
        }

        ContentParser *get_pre_content_parser() const
        {
            return ptr_of(pre_content_parser_);
        }

    public: // task
        void set_task(HttpTask *task);

        void set_task(std::shared_ptr<HttpTask> task)
        {
            task_ = std::move(task);
        }

        HttpTask *get_task() const
        {
            return ptr_of(task_);
        }

    public: // original file name
        void set_original_file_name(std::string name)
        {
            original_file_name_ = std::move(name);
        }

        const std::string &get_original_file_name() const
        {
            return original_file_name_;
        }

        bool is_downloading() const
        {
            return is_download_file_;
        }

        bool is_uploading() const
        {
            return is_upload_file_ && task_;
        }

        void set_download_file(bool flag)
        {
            is_download_file_ = flag;
        }

        void set_upload_file(bool flag)
        {
            is_upload_file_ = flag;
        }

        bool is_task_prepared() const
        {
            return task_ && (is_download_file_ ? task_->get_task_type() == HttpTaskType::download_file_ : task_->get_task_type() == HttpTaskType::upload_file_);
        }

        bool is_pending_large_block() const
        {
            return is_download_file_ || is_upload_file_;
        }

        std::string get_peer_ip() const;

        uint32_t get_peer_ip_uint32() const;

        static size_t get_max_packet_size();

        // 直接访问headers（中间件等需要）
        const std::unordered_map<std::string, std::string> &headers() const
        {
            return headers_;
        }

    protected:
        HttpVersion version_ = HttpVersion::v_1_1;
        bool is_good_;
        bool is_upload_file_ = false;
        bool is_download_file_ = false;
        ResponseCode error_code_;
        ContentType content_type_;
        uint32_t body_length_ = 0;
        HttpSessionContext *context_;
        std::unique_ptr<HttpPacketParser> parser_;
        std::unordered_map<std::string, std::vector<std::string> > params_;
        std::unordered_map<std::string, std::string> headers_;
        std::string content_type_text_;
        std::unordered_map<std::string, std::string> content_type_extra_;
        std::unique_ptr<Content> body_content_;
        ::yuan::buffer::ByteBuffer buffer_;
        ::yuan::buffer::ByteBuffer input_cache_;
        std::shared_ptr<ContentParser> pre_content_parser_;
        std::shared_ptr<HttpTask> task_;
        std::string chunked_checksum_;
        std::string original_file_name_;
        std::filesystem::path body_file_path_;
        std::unique_ptr<std::ofstream> body_file_stream_;
        std::uint32_t body_file_expected_ = 0;
        std::uint32_t body_file_received_ = 0;
        bool body_file_owned_ = false;
    };
}
#endif
