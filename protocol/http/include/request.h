#ifndef __NET_HTTP_REQUEST_H__
#define __NET_HTTP_REQUEST_H__

#include <string>
#include <string_view>
#include <vector>

#include "packet.h"

namespace yuan::net::http 
{ 
    enum class HttpMethod : char
    {
        invalid_ = -1,
        get_,
        post_,
        put_,
        delete_,
        options_,
        head_,
        comment_,
        trace_,
        patch_,
        propfind_,
        proppatch_,
        mkcol_,
        copy_,
        move_,
        lock_,
        unlock_,
        report_,
        acl_,
        search_,
    };

    class HttpRequest : public HttpPacket
    {
        friend class HttpRequestParser;
    public:
        HttpRequest(HttpSessionContext *context_);
        ~HttpRequest();

        // 禁用拷贝
        HttpRequest(const HttpRequest&) = delete;
        HttpRequest& operator=(const HttpRequest&) = delete;

    public:
        virtual void reset();

        virtual bool pack_header(Connection *conn = nullptr);

        virtual PacketType get_packet_type() { return PacketType::request; }

    public:
        HttpMethod get_method() const { return method_; }
        
        std::string_view get_method_string() const;
        std::string get_raw_method() const;

        void set_method(HttpMethod method) { method_ = method; }

        const std::vector<std::string> & get_url_domain() const { return url_domain_; }

        const std::string & get_raw_url() const  { return url_; }

        void set_raw_url(std::string url) { url_ = std::move(url); }

        std::string get_last_uri();
        
        // 获取路径（不含query string）
        std::string_view get_path() const;

        // 获取query string
        std::string_view get_query_string() const;

        // 获取参数快捷方法
        std::string get_param(const std::string &key, const std::string &default_val = "") const;
        int get_param_int(const std::string &key, int default_val = 0) const;

        // Cookie解析
        const std::unordered_map<std::string, std::string> & cookies() const;
        std::string get_cookie(const std::string &name) const;

        // 判断是否为指定方法
        bool is_get() const { return method_ == HttpMethod::get_; }
        bool is_post() const { return method_ == HttpMethod::post_; }
        bool is_put() const { return method_ == HttpMethod::put_; }
        bool is_delete() const { return method_ == HttpMethod::delete_; }
        bool is_options() const { return method_ == HttpMethod::options_; }
        bool is_head() const { return method_ == HttpMethod::head_; }
        bool is_patch() const { return method_ == HttpMethod::patch_; }
        bool is_propfind() const { return method_ == HttpMethod::propfind_; }
        bool is_proppatch() const { return method_ == HttpMethod::proppatch_; }
        bool is_mkcol() const { return method_ == HttpMethod::mkcol_; }
        bool is_copy() const { return method_ == HttpMethod::copy_; }
        bool is_move() const { return method_ == HttpMethod::move_; }
        bool is_lock() const { return method_ == HttpMethod::lock_; }
        bool is_unlock() const { return method_ == HttpMethod::unlock_; }
        bool is_report() const { return method_ == HttpMethod::report_; }
        bool is_acl() const { return method_ == HttpMethod::acl_; }
        bool is_search() const { return method_ == HttpMethod::search_; }

    private:
        HttpMethod method_;
        std::string url_;
        std::vector<std::string> url_domain_;
        mutable std::unordered_map<std::string, std::string> parsed_cookies_;
        mutable bool cookies_parsed_ = false;
    };
}
#endif
