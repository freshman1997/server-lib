#ifndef __YUAN_SERVER_PLUGIN_HTTP_BRIDGE_H__
#define __YUAN_SERVER_PLUGIN_HTTP_BRIDGE_H__

namespace yuan::app
{
class PluginHostService;
}

namespace yuan::server
{
class HttpService;

bool install_plugin_http_bridge(yuan::app::PluginHostService &plugin_host, HttpService &http_service);

} // namespace yuan::server

#endif
