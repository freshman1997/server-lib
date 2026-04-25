#ifndef __NET_WEBDAV_XML_H__
#define __NET_WEBDAV_XML_H__

#include "webdav_lock_manager.h"
#include "webdav_resource.h"

#include <string>
#include <string_view>
#include <vector>

namespace yuan::net::webdav
{
    std::string xml_escape(std::string_view text);
    Depth parse_depth(std::string_view text, Depth fallback);
    bool parse_overwrite(std::string_view text);
    std::vector<Property> parse_property_names(std::string_view body);
    std::string parse_lock_owner(std::string_view body);
    LockScope parse_lock_scope(std::string_view body);
    std::string format_http_date(std::chrono::system_clock::time_point tp);
    std::string multistatus_xml(const std::vector<PropertyResult> &results);
    std::string lockdiscovery_xml(const std::vector<LockInfo> &locks);
    std::string propfind_response_xml(const std::string &href,
                                      const ResourceInfo &info,
                                      const std::vector<Property> &dead_properties,
                                      const Quota &quota,
                                      const std::vector<LockInfo> &locks);
}

#endif
