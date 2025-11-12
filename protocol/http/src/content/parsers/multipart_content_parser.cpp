#include "content/parsers/multipart_content_parser.h"
#include "header_util.h"
#include "base/utils/base64.h"
#include "content/types.h"
#include "content_type.h"
#include "packet.h"
#include "header_key.h"
#include "ops/option.h"
#include <cstdlib>
#include <fstream>
#include <iostream>

namespace yuan::net::http 
{
    bool MultipartFormDataParser::can_parse(ContentType contentType)
    {
        return contentType == ContentType::multpart_form_data;
    }

    bool MultipartFormDataParser::parse(HttpPacket *packet)
    {
        const char *begin = packet->body_begin();
        const char *end = packet->body_end();
        const auto &contentExtra = packet->get_content_type_extra();
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
        fd->type = helper::dispistion_type_names[(std::size_t)helper::ContentDispositionType::form_data_];
        Content *content = new Content(ContentType::multpart_form_data, fd);
        while (begin <= end)
        {
            begin += boundaryStart.size();
            begin += helper::skip_new_line(begin);

            auto dis = parse_content_disposition(begin, end);
            if (dis.first == 0) {
                delete content;
                return false;
            }

            helper::ContentDispositionType disType = helper::get_content_disposition_type(dis.second.first);
            if (disType != helper::ContentDispositionType::form_data_) {
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
                fd->properties[pit->second] = std::make_shared<FormDataStringItem>(res.second);
            } else {
                StreamResult result;
                int ret = parse_part_file_content(result, packet, begin, end, fit->second);
                if (ret < 0 || result.type_.empty()) {
                    delete content;
                    return false;
                }

                const auto &extra = result.extra_;
                auto tIt = extra.find("____tmp_file_name");
                if (tIt == extra.end()) {
                    std::pair<std::string, std::unordered_map<std::string, std::string>> type = {result.type_, result.extra_ };
                    auto it = contentExtra.find("boundary");
                    if (it == contentExtra.end()) {
                        delete content;
                        return false;
                    }

                    // ----boundary--
                    fd->properties[pit->second] = std::make_shared<FormDataStreamItem>(fit->second, type, result.stream_begin_, result.stream_end_);
                } else {
                    fd->properties[pit->second] = std::make_shared<FormDataFileItem>(fit->second, tIt->second, extra);
                }

                begin += result.len_;
            }

            begin += helper::skip_new_line(begin);
            if (helper::str_cmp(begin, begin + boundaryEnd.size(), boundaryEnd.c_str())) {
                break;
            }
        }

        packet->set_body_content(content);

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

    int MultipartFormDataParser::parse_part_file_content(StreamResult &result, HttpPacket *packet, const char *begin, const char *end, const std::string &originName)
    {
        const char *p = begin;
        ContentType ctype;
        std::unordered_map<std::string, std::string> extra;
        std::string ctypeName;
        while (begin <= end && *begin != ':') {
            ctypeName.push_back(std::tolower(*begin));
            ++begin;
        }

        if (ctypeName != http_header_key::content_type) {
            return -1;
        }

        ++begin;
        if (*begin == ' ') {
            ++begin;
        }

        std::string ctypeText;
        const auto &res = packet->parse_content_type(begin, end, ctypeText, extra);
        if (!res.first || ctypeText.empty()){
            return -1;
        }

        begin += res.second;
        begin += helper::skip_new_line(begin);

        std::fstream *file = nullptr;
        if (config::form_data_upload_save) {
            const std::string &tmpName = get_random_filename(originName);
            file = new std::fstream(tmpName.c_str(), std::ios_base::out);
            if (!file->good()) {
                return -1;
            }
            extra["____tmp_file_name"] = tmpName;
        }

        const auto &contentExtra = packet->get_content_type_extra();
        auto it = contentExtra.find("boundary");
        if (it == contentExtra.end()) {
            return -1;
        }

        const std::string &boundaryStart = "--" + it->second;
        if (end - begin < boundaryStart.size()) {
            return -1;
        }
        
        const char *from = begin;
        while (begin != end) {
            if (helper::str_cmp(begin, begin + boundaryStart.size(), boundaryStart.c_str())) {
                break;
            }
            ++begin;
        }

        if (begin + 2 > end) {
            return -1;
        }

        if (file) {
            file->write(from, (begin - from) - 2);
            file->flush();
            file->close();
            delete file;
        }

        result.type_ = ctypeText;
        result.extra_ = extra;
        result.len_ = begin - p;
        result.stream_begin_ = from;
        result.stream_end_ = begin - 2;

        return 0;
    }

    bool MultipartByterangesParser::can_parse(ContentType contentType)
    {
        return contentType == ContentType::multpart_byte_ranges;
    }

    bool MultipartByterangesParser::parse(HttpPacket *packet)
    {
        if (packet->get_body_content()) {
            return false;
        }
        
        const char *begin = packet->body_begin();
        const char *end = packet->body_end();

        const auto &contentExtra = packet->get_content_type_extra();
        auto it = contentExtra.find("boundary");
        if (it == contentExtra.end()) {
            return false;
        }
        
        std::string boundaryStart = "--" + it->second;
        if (end - begin < boundaryStart.size()) {
            return false;
        }

        std::string boundaryEnd = "--" + it->second + "--";
        RangeDataContent *rangeContent = new RangeDataContent;
        Content *content = new Content(ContentType::multpart_form_data, rangeContent);
        
        while (begin <= end)
        {
            // skip boundary
            if (!helper::str_cmp(begin, begin + boundaryStart.size(), boundaryStart.c_str())) {
                delete content;
                return false;
            }

            begin += boundaryStart.size();
            begin += helper::skip_new_line(begin);

            RangeDataItem *item = new RangeDataItem;
            rangeContent->contents.push_back(item);

            const auto &res = packet->parse_content_type(begin, end, item->content_type_.first, item->content_type_.second);
            if (!res.first || item->content_type_.first.empty()){
                delete content;
                return false;
            }

            begin += res.second;
            begin += helper::skip_new_line(begin);

            const auto &rangeRes = parse_content_range(begin, end);
            if (!std::get<0>(rangeRes)) {
                delete content;
                return false;
            }

            begin += std::get<4>(rangeRes);

            item->chunk.from = std::get<1>(rangeRes);
            item->chunk.to = std::get<2>(rangeRes);
            item->chunk.length = std::get<3>(rangeRes);
            item->chunk.content.begin = begin;

            if (item->chunk.from > item->chunk.to || item->chunk.length == 0) {
                delete content;
                return false;
            }

            begin += item->chunk.to - item->chunk.from;
            item->chunk.content.end = begin;
            
            begin += helper::skip_new_line(begin);

            if (helper::str_cmp(begin, begin + boundaryEnd.size(), boundaryEnd.c_str())) {
                break;
            }
        }

        packet->set_body_content(content);

        return true;
    }

    std::tuple<bool, uint32_t, uint32_t, uint32_t, uint32_t> MultipartByterangesParser::parse_content_range(const char *begin, const char *end)
    {
        const char *p = begin;
        if (!helper::str_cmp(p, p + 13, http_header_key::content_range)) {
            return {false, 0, 0, 0, 0};
        }

        p += 13;

        if (*p == ':') {
            ++p;
        }

        p += helper::skip_new_line(p);

        if (!helper::str_cmp(p, p + 5, "bytes")) {
            return {false, 0, 0, 0, 0};
        }

        p += 5;
        p += helper::skip_new_line(p);

        std::string from;
        helper::read_next(p, end, '-', from);

        p += from.size() + (from.empty() ? 0 : 1);

        std::string to;
        helper::read_next(p, end, '/', to);
        if (to.empty()) {
            return {false, 0, 0, 0, 0};
        }

        p += to.size() + 1;

        std::string sz;
        helper::read_next(p, end, '\r', sz);
        if (sz.empty()) {
            return {false, 0, 0, 0, 0};
        }

        p += sz.size() + 2;

        return {true, std::atoi(from.c_str()), std::atoi(to.c_str()), std::atoi(sz.c_str()), p - begin};
    }
}