#include "ops/option.h"
#include "ops/config_manager.h"

#define KEY_TO_STRING(key) (#key)

namespace yuan::net::http::config
{
    int connection_idle_timeout = 30 * 1000;
    
    const char * config_file_name = "http.json";

    const char * server_name = KEY_TO_STRING(server_name);

    const char * parse_form_data_content_types = KEY_TO_STRING(parse_form_data_content_types);

    const char * static_file_paths = KEY_TO_STRING(static_file_paths);
    const char * static_file_paths_root = "root";
    const char * static_file_paths_path = "path";

    const char * playable_types = KEY_TO_STRING(playable_types);

    // 最大包体长度默认 2 m
    uint32_t max_header_length = 1024 * 1024;

    uint32_t client_max_content_length = 1024 * 1024 * 2;

    bool close_idle_connection = false;

    bool form_data_upload_save = false;

    int proxy_connect_timeout = 5 * 1000;

    int proxy_max_pending = 10;

    // 代理最大缓冲区
    int proxy_buffer_max = 1024 * 1024 * 3;

    void load_config()
    {
        auto cfgManager = HttpConfigManager::get_instance();
        if (!cfgManager->good()) {
            return;
        }

        connection_idle_timeout = cfgManager->get_uint_property(KEY_TO_STRING(connection_idle_timeout), connection_idle_timeout);
        client_max_content_length = cfgManager->get_uint_property(KEY_TO_STRING(max_content_length), client_max_content_length);
        max_header_length = cfgManager->get_uint_property(KEY_TO_STRING(max_header_length), max_header_length);
        close_idle_connection = cfgManager->get_uint_property(KEY_TO_STRING(close_idle_connection), close_idle_connection);
        form_data_upload_save = cfgManager->get_bool_property(KEY_TO_STRING(form_data_upload_save), form_data_upload_save);
    }
}
