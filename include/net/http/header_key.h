#ifndef __HEADER_KEY_H__
#define __HEADER_KEY_H__

/**
 * Note. 当前处理的 header key 全部使用小写，http 规范要求大小写不敏感
 */
namespace net::http::http_header_key 
{
    extern const char *content_type;
    extern const char *content_length;
    extern const char *content_range;
    extern const char *accept;                 // text/html, application/xhtml+xml, application/xml;q=0.9, */*;q=0.8 
    extern const char *accept_ch;              // 
    extern const char *accept_language;
    extern const char *accept_charset;
    extern const char *accept_encoding;
    extern const char *user_agent;
    extern const char *host;
    extern const char *cookie;
    extern const char *authorization;
    extern const char *date;
    extern const char *x_cache;
    extern const char *vary;
    extern const char *via;
    extern const char *origin;           
    extern const char *referer;


    extern const char *connection;
    extern const char *server;
    extern const char *range;
    extern const char *status;                 // 状态码
}

#endif
