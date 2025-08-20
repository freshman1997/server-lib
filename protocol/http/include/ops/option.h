#ifndef __HTTP_SERVER_OPTION_H__
#define __HTTP_SERVER_OPTION_H__
#include <cstdint>
#include <string_view>

namespace yuan::net::http::config
{  
    // 配置文件路径
    extern const char * config_file_name;

    /* config key names start */

    // server 名称
    extern const char * server_name;

    // form-data 需要解析的 content 类型，如 application/json
    extern const char * parse_form_data_content_types;

    // 解析为静态文件的路径
    extern const char * static_file_paths;
    // root
    extern const char * static_file_paths_root;
    // path
    extern const char * static_file_paths_path;

    // 视为分片可播放的后缀列表，静态文件
    extern const char * playable_types;

    /* config key names end */


    /* default config value start */


    // 最大头部长度
    extern uint32_t max_header_length;

    // 默认最大包体长度
    extern uint32_t client_max_content_length;

      // 连接空闲时长
    extern int connection_idle_timeout;

    extern bool close_idle_connection;

    // 上传的文件是否保存到文件中
    extern bool form_data_upload_save;

    /* default config value end */

    extern int proxy_connect_timeout;

    extern int proxy_max_pending;

    extern int proxy_buffer_max;

    extern const std::string_view file_list_html_text;

    void load_config();
}

#endif