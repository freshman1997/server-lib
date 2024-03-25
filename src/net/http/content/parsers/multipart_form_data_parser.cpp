#include "net/http/content/parsers/multipart_form_data_parser.h"
#include "base/utils/base64.h"
#include "net/http/content/types.h"
#include "net/http/content_type.h"
#include "net/http/request.h"
#include "net/http/header_key.h"

namespace net::http 
{
    namespace helper
    {
        std::unordered_map<std::string, ContentDispositionType> dispistion_type_mapping_ = {
            {"inline", ContentDispositionType::inline_},
            {"attachment", ContentDispositionType::attachment_},
            {"form-data", ContentDispositionType::form_data_},
        };

        const char * dispistion_type_names[] = {
            "inline",
            "attachment",
            "form-data"
        };

        std::string filename_ = "filename";
        std::string name_ = "name";

        static bool str_cmp(const char *begin, const char *end, const char *str)
        {
            const char *p = begin;
            const char *p1 = str;
            for (;p != end && *p1; ++p, ++p1) {
                if (std::tolower(*p) != *p1) {
                    return false;
                }
            }

            return p == end && !(*p1);
        }

        static std::string read_identifier(const char *p, const char *end)
        {
            std::string id;
            bool quoted = false;
            bool ended = false;
            for (; p != end; ++p) {
                char ch = *p;
                if (ch == ' ') {
                    continue;
                }

                if (ch == '\"') {
                    if (quoted) break;
                    quoted = true;
                    continue;
                }

                if (ch == '=' || ch == ':' || ch == ';' || ch == '\r') {
                    break;
                }

                id.push_back(std::tolower(ch));
            }

            return id;
        }

        static uint32_t skip_new_line(const char *data)
        {
            char ch = *data;
            if (ch == '\r') {
                return 2;
            }

            if (ch == '\n') {
                return 1;
            }

            return 0;
        }
    }

    ContentDispositionType get_content_disposition_type(const std::string &name)
    {
        auto it = helper::dispistion_type_mapping_.find(name);
        return it == helper::dispistion_type_mapping_.end() ? ContentDispositionType::unknow_ : it->second;
    }

    std::string content_disposition_to_string(ContentDispositionType type)
    {
        if (type == ContentDispositionType::unknow_) {
            return {};
        }

        return helper::dispistion_type_names[(std::size_t)type];
    }

    bool MultipartFormDataParser::can_parse(const content_type contentType)
    {
        return contentType == content_type::multpart_form_data;
    }

    bool MultipartFormDataParser::parse(HttpRequest *req)
    {
        const char *begin = req->body_begin();
        const char *end = req->body_end();
        const auto &contentExtra = req->get_content_type_extra();
        auto it = contentExtra.find("boundary");
        if (it == contentExtra.end()) {
            return false;
        }

        // skip boundary
        std::string boundaryStart = "--" + it->second;
        if (end - begin < boundaryStart.size()) {
            return false;
        }

        std::string boundaryEnd = "--" + it->second + "--";
        FormDataContent *fd = new FormDataContent;
        fd->type = helper::dispistion_type_names[(std::size_t)ContentDispositionType::form_data_];
        Content *content = new Content(content_type::multpart_form_data, fd);
        while (begin != end)
        {
            begin += boundaryStart.size();
            begin += helper::skip_new_line(begin);

            auto dis = parse_content_disposition(begin, end);
            if (dis.first == 0) {
                delete content;
                return false;
            }

            ContentDispositionType disType = get_content_disposition_type(dis.second.first);
            if (disType != ContentDispositionType::form_data_) {
                delete content;
                return false;
            }

            begin += dis.first;
            begin += helper::skip_new_line(begin);

            auto pit = dis.second.second.find(helper::name_);
            if (pit == dis.second.second.end()) {
                delete content;
                return false;
            }

            auto fit = dis.second.second.find(helper::filename_);
            begin += helper::skip_new_line(begin);
            if (fit == dis.second.second.end()) {
                const auto &res = parse_part_value(begin, end);
                begin += res.first;
                fd->properties[pit->second] = FormDataStringItem(res.second);
            } else {
                const auto &res = parse_part_file_content(req, begin, end, fit->second);
                if (std::get<0>(res).empty()) {
                    delete content;
                    return false;
                }

                fd->properties[fit->second] = FormDataStreamItem(fit->second, 
                    {std::get<0>(res), std::get<1>(res)}, 
                    begin, begin + std::get<2>(res));

                begin += std::get<2>(res);
            }

            begin += helper::skip_new_line(begin);
            if (helper::str_cmp(begin, begin + boundaryEnd.size(), boundaryEnd.c_str())) {
                break;
            }
        }

        req->set_body_content(content);

        return true;
    }

    ContentDisposition MultipartFormDataParser::parse_content_disposition(const char *begin, const char *end)
    {
        ContentDisposition res = {0, {}};
        const char *p = begin;
        if (p + 21 > end) {
            return res;
        }

        if (!helper::str_cmp(p, p + 20, "content-disposition:")) {
            return res;
        }

        p += 20;

        std::string type;
        for (; p <= end; ++p) {
            char ch = *p;
            if (ch == ' ') {
                continue;
            }

            if (ch == ';') {
                ++p;
                break;
            }
            type.push_back(std::tolower(ch));
        }

        std::unordered_map<std::string, std::string> &kvs = res.second.second;
        while (p <= end) {
            const auto &k = helper::read_identifier(p, end);
            if (k.empty()) {
                return res;
            }

            p += k.size() + 2;
            const auto &v = helper::read_identifier(p, end);
            if (v.empty()) {
                return res;
            }

            kvs[k] = v;
            p += v.size() + 2;
            if (*p == ';') {
                ++p;
            } else {
                break;
            }
        }

        if (kvs.empty()) {
            return res;
        }

        res.first = p - begin;
        res.second.first = type;

        return res;
    }

    std::pair<uint32_t, std::string> MultipartFormDataParser::parse_part_value(const char *begin, const char *end)
    {
        const char *p = begin;
        std::string val;
        while (p != end)
        {
            char ch = *p;
            if (ch == '\r') {
                break;
            }
            val.push_back(ch);
            ++p;
        }

        return {p - begin, val};
    }

    static std::string get_random_filename(const std::string &origin)
    {
        std::string ext;
        std::size_t pos = origin.find_last_of(".");
        if (pos != std::string::npos) {
            ext = origin.substr(pos);
        }

        time_t now = time(nullptr) + rand() % 0x431243;
        const std::string &b64 = base::util::base64_encode(origin);
        return std::to_string(now) + b64.substr(0, b64.size() - 2) + ext;
    }

    std::tuple<std::string, std::unordered_map<std::string, std::string>, uint32_t> MultipartFormDataParser::parse_part_file_content(
        HttpRequest *req, const char *begin, const char *end, const std::string &originName)
    {
        const char *p = begin;
        content_type ctype;
        std::unordered_map<std::string, std::string> extra;
        std::string ctypeName;
        while (begin <= end && *begin != ':') {
            ctypeName.push_back(std::tolower(*begin));
            ++begin;
        }

        if (ctypeName != http_header_key::content_type) {
            return {{}, {}, 0};
        }

        ++begin;
        if (*begin == ' ') {
            ++begin;
        }

        std::string ctypeText;
        const auto &res = req->parse_content_type(begin, end, ctypeText, extra);
        if (!res.first || ctypeText.empty()){
            return {{}, {}, 0};
        }

        begin += res.second;
        begin += helper::skip_new_line(begin);

        const auto &contentExtra = req->get_content_type_extra();
        auto it = contentExtra.find("boundary");
        const std::string &boundaryStart = "--" + it->second;
        while (begin != end) {
            if (helper::str_cmp(begin, begin + boundaryStart.size(), boundaryStart.c_str())) {
                break;
            }

            ++begin;
        }

        return {ctypeText, extra, begin - p};
    }
}