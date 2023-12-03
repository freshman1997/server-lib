#include "net/http/header_key.h"

namespace net::http::http_header_key  {
    const char *content_type     = "content-type";           // 内容类型
    const char *content_length   = "content-length";         // body 长度
    const char *accept           = "accept";                 // text/html, application/xhtml+xml, application/xml;q=0.9, */*;q=0.8 
    const char *accept_ch        = "accept-ch";              // 
    const char *accept_language  = "accept-language";
    const char *accept_charset   = "accept-charset";
    const char *accept_encoding  = "accept-encoding";
    const char *user_agent       = "user-agent";
    const char *host             = "host";
    const char *cookie           = "cookie";
    const char *authorization    = "authorization";
    const char *date             = "date";
    const char *x_cache          = "x-cache";
    const char *vary             = "vary";
    const char *via              = "via";
    const char *origin           = "";
    const char *referer          = "referer";


    const char *connection       = "connection";
    const char *server           = "server";
    const char *range            = "range";
    const char *status           = "status";                 // 状态码
}