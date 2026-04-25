#include "webdav_xml.h"

#include <algorithm>
#include <cctype>
#include <iomanip>
#include <sstream>

namespace yuan::net::webdav
{
    std::string xml_escape(std::string_view text)
    {
        std::string out;
        out.reserve(text.size());
        for (char ch : text) {
            switch (ch) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            case '\'': out += "&apos;"; break;
            default: out.push_back(ch); break;
            }
        }
        return out;
    }

    Depth parse_depth(std::string_view text, Depth fallback)
    {
        if (text == "0") return Depth::zero;
        if (text == "1") return Depth::one;
        if (text == "infinity" || text == "Infinity") return Depth::infinity;
        return fallback;
    }

    bool parse_overwrite(std::string_view text)
    {
        return text.empty() || text == "T" || text == "t" || text == "true" || text == "1";
    }

    std::vector<Property> parse_property_names(std::string_view body)
    {
        std::vector<Property> props;
        constexpr std::string_view names[] = {
            "displayname", "getcontentlength", "getcontenttype", "getetag", "getlastmodified",
            "creationdate", "resourcetype", "supportedlock", "lockdiscovery",
            "quota-used-bytes", "quota-available-bytes"
        };
        for (auto name : names) {
            if (body.empty() || body.find(name) != std::string_view::npos || body.find("allprop") != std::string_view::npos) {
                props.push_back(Property{ "DAV:", std::string(name), {} });
            }
        }
        return props;
    }

    std::string parse_lock_owner(std::string_view body)
    {
        const auto b = body.find("<D:owner");
        const auto start = b == std::string_view::npos ? body.find("<owner") : b;
        if (start == std::string_view::npos) {
            return {};
        }
        const auto gt = body.find('>', start);
        const auto end = body.find("owner>", gt == std::string_view::npos ? start : gt);
        if (gt == std::string_view::npos || end == std::string_view::npos || end <= gt) {
            return {};
        }
        return std::string(body.substr(gt + 1, end - gt - 3));
    }

    LockScope parse_lock_scope(std::string_view body)
    {
        return body.find("shared") != std::string_view::npos ? LockScope::shared : LockScope::exclusive;
    }

    std::string format_http_date(std::chrono::system_clock::time_point tp)
    {
        const auto t = std::chrono::system_clock::to_time_t(tp);
        std::tm tm{};
#ifdef _WIN32
        gmtime_s(&tm, &t);
#else
        gmtime_r(&t, &tm);
#endif
        std::ostringstream oss;
        oss << std::put_time(&tm, "%a, %d %b %Y %H:%M:%S GMT");
        return oss.str();
    }

    std::string lockdiscovery_xml(const std::vector<LockInfo> &locks)
    {
        std::string xml;
        for (const auto &lock : locks) {
            xml += "<D:activelock><D:locktype><D:write/></D:locktype><D:lockscope>";
            xml += lock.scope == LockScope::exclusive ? "<D:exclusive/>" : "<D:shared/>";
            xml += "</D:lockscope><D:depth>";
            xml += lock.depth == Depth::zero ? "0" : lock.depth == Depth::one ? "1" : "infinity";
            xml += "</D:depth><D:owner>" + xml_escape(lock.owner) + "</D:owner>";
            xml += "<D:locktoken><D:href>" + xml_escape(lock.token) + "</D:href></D:locktoken></D:activelock>";
        }
        return xml;
    }

    std::string propfind_response_xml(const std::string &href,
                                      const ResourceInfo &info,
                                      const std::vector<Property> &dead_properties,
                                      const Quota &quota,
                                      const std::vector<LockInfo> &locks)
    {
        std::string prop;
        prop += "<D:displayname>" + xml_escape(info.display_name) + "</D:displayname>";
        prop += "<D:resourcetype>";
        if (info.is_collection) {
            prop += "<D:collection/>";
        }
        prop += "</D:resourcetype>";
        prop += "<D:getcontentlength>" + std::to_string(info.content_length) + "</D:getcontentlength>";
        prop += "<D:getcontenttype>" + xml_escape(info.content_type) + "</D:getcontenttype>";
        prop += "<D:getetag>" + xml_escape(info.etag) + "</D:getetag>";
        prop += "<D:getlastmodified>" + format_http_date(info.last_modified) + "</D:getlastmodified>";
        prop += "<D:quota-used-bytes>" + std::to_string(quota.used_bytes) + "</D:quota-used-bytes>";
        prop += "<D:quota-available-bytes>" + std::to_string(quota.available_bytes) + "</D:quota-available-bytes>";
        prop += "<D:supportedlock><D:lockentry><D:lockscope><D:exclusive/></D:lockscope><D:locktype><D:write/></D:locktype></D:lockentry>";
        prop += "<D:lockentry><D:lockscope><D:shared/></D:lockscope><D:locktype><D:write/></D:locktype></D:lockentry></D:supportedlock>";
        prop += "<D:lockdiscovery>" + lockdiscovery_xml(locks) + "</D:lockdiscovery>";
        for (const auto &p : dead_properties) {
            prop += "<D:" + p.name + ">" + xml_escape(p.value) + "</D:" + p.name + ">";
        }
        return "<D:response><D:href>" + xml_escape(href) + "</D:href><D:propstat><D:prop>" +
               prop + "</D:prop><D:status>HTTP/1.1 200 OK</D:status></D:propstat></D:response>";
    }

    std::string multistatus_xml(const std::vector<PropertyResult> &results)
    {
        std::string xml = "<?xml version=\"1.0\" encoding=\"utf-8\"?><D:multistatus xmlns:D=\"DAV:\">";
        for (const auto &r : results) {
            xml += "<D:response><D:href>" + xml_escape(r.href) + "</D:href><D:propstat><D:prop>";
            for (const auto &p : r.properties) {
                xml += "<D:" + p.name + ">" + xml_escape(p.value) + "</D:" + p.name + ">";
            }
            xml += "</D:prop><D:status>HTTP/1.1 " + std::to_string(r.status) + " OK</D:status></D:propstat></D:response>";
        }
        xml += "</D:multistatus>";
        return xml;
    }
}
