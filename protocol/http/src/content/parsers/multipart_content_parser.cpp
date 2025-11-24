#include "content/parsers/multipart_content_parser.h"
#include "base/utils/base64.h"
#include "content/types.h"
#include "content_type.h"
#include "header_key.h"
#include "header_util.h"
#include "ops/option.h"
#include "packet.h"
#include <cstdlib>
#include <fstream>
#include <io.h>
#include <iostream>

namespace yuan::net::http
{
    bool MultipartFormDataParser::can_parse(ContentType contentType)
    {
        return contentType == ContentType::multpart_form_data;
    }

    bool MultipartFormDataParser::parse(HttpPacket *packet)
    {
        auto &reader = packet->get_buffer_reader();

        const auto &contentExtra = packet->get_content_type_extra();
        const auto it = contentExtra.find("boundary");
        if (it == contentExtra.end()) {
            return false;
        }

        // skip boundary
        const auto &boundary = it->second;
        if (boundary.empty()) {
            return false;
        }

        const std::string boundaryStart = "--" + boundary + "\r\n";
        if (reader.readable_bytes() < boundaryStart.size()) {
            return false;
        }

        const std::string boundaryEnd = "--" + boundary + "--";
        auto *fd = new FormDataContent;
        fd->type = helper::dispistion_type_names[static_cast<std::size_t>(helper::ContentDispositionType::form_data_)];
        auto *content = new Content(ContentType::multpart_form_data, fd);
        if (!reader.read_match_ignore_case(boundaryStart.data())) {
            std::cout << reader.read_from_offset(reader.get_read_offset(), boundaryStart.size()) << std::endl;
            delete content;
            return false;
        }

        const std::string contentStart = "\r\n--" + boundary;

        while (reader.readable_bytes() > 0)
        {
            auto dis = parse_content_disposition(reader);
            if (dis.first == 0) {
                std::cout << reader.read_from_offset(reader.get_read_offset(), 20) << std::endl;
                delete content;
                return false;
            }

            helper::ContentDispositionType disType = helper::get_content_disposition_type(dis.second.first);
            if (disType != helper::ContentDispositionType::form_data_) {
                delete content;
                return false;
            }

            auto pit = dis.second.second.find(helper::name_);
            if (pit == dis.second.second.end()) {
                delete content;
                return false;
            }

            if (!reader.skip_newline_symbol()) {
                delete content;
                return false;
            }

            if (const auto fit = dis.second.second.find(helper::filename_); fit == dis.second.second.end()) {
                if (!reader.skip_newline_symbol()) {
                    delete content;
                    return false;
                }

                const auto &res = parse_part_value(reader, contentStart);
                if (res.first < 0) {
                    delete content;
                    return false;
                }
                fd->properties[pit->second] = std::make_shared<FormDataStringItem>(res.second);
            } else {
                StreamResult result;
                if (const int ret = parse_part_file_content(result, packet, reader, fit->second, contentStart); ret < 0 || result.type_.empty()) {
                    delete content;
                    return false;
                }

                const auto &extra = result.extra_;
                if (const auto tIt = extra.find("____tmp_file_name"); tIt == extra.end()) {
                    std::pair<std::string, std::unordered_map<std::string, std::string>> type = {result.type_, result.extra_ };
                    if (!contentExtra.contains("boundary")) {
                        delete content;
                        return false;
                    }

                    // ----boundary--
                    fd->properties[pit->second] = std::make_shared<FormDataStreamItem>(fit->second, type, result.stream_begin_, result.len_);
                } else {
                    fd->properties[pit->second] = std::make_shared<FormDataFileItem>(fit->second, tIt->second, extra);
                }
            }

            if (const std::string tmp = reader.read_from_offset(reader.get_read_offset() - boundary.size() - 2,  boundaryEnd.size(), true); tmp == boundaryEnd) {
                break;
            }

            if (!reader.skip_newline_symbol()) {
                delete content;
                return false;
            }
        }

        packet->set_body_content(content);

        return true;
    }

    ContentDisposition MultipartFormDataParser::parse_content_disposition(buffer::BufferReader &reader)
    {
        ContentDisposition res = {0, {}};
        if (21 > reader.readable_bytes()) {
            return res;
        }

        const size_t from = reader.get_read_offset();
        if (!reader.read_match_ignore_case("content-disposition:")) {
            return res;
        }

        std::string type;
        for (char ch = reader.read_char(); reader; ch = reader.read_char()) {
            if (ch == ' ') {
                continue;
            }

            if (ch == ';') {
                ++reader;
                break;
            }
            type.push_back(std::tolower(ch));
        }

        std::unordered_map<std::string, std::string> &kvs = res.second.second;
        while (reader.readable_bytes() > 0) {
            const auto &k = helper::read_identifier(reader);
            if (k.empty()) {
                return res;
            }

            if (!reader.read_match("=")) {
                return res;
            }

            const auto &v = helper::read_identifier(reader);
            if (v.empty()) {
                return res;
            }

            kvs[k] = v;
            if (*reader == ';') {
                ++reader;
            } else {
                break;
            }
        }

        if (kvs.empty()) {
            return res;
        }

        res.first = reader.get_read_offset() - from;
        res.second.first = type;

        return res;
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

    std::pair<int, std::string> MultipartFormDataParser::parse_part_value(buffer::BufferReader &reader, const std::string &boundary)
    {
        if (reader.readable_bytes() < boundary.size()) {
            return {-1, {}};
        }

        const size_t from = reader.get_read_offset();
        int len = reader.find_match_ignore_case(boundary);
        if (len < 0) {
            return {-1, {}};
        }

        return {len, reader.read_from_offset(from, len)};
    }

    int MultipartFormDataParser::parse_part_file_content(StreamResult &result, const HttpPacket *packet, buffer::BufferReader &reader, const std::string &originName, const std::string &boundary)
    {
        std::unordered_map<std::string, std::string> extra;
        std::string ctypeName;
        while (reader.readable_bytes() > 0 && *reader != ':') {
            ctypeName.push_back(std::tolower(*reader));
            ++reader;
        }

        if (ctypeName != http_header_key::content_type) {
            return -1;
        }

        ++reader;
        if (*reader == ' ') {
            ++reader;
        }

        std::string ctypeText;
        const auto &res = HttpPacket::parse_content_type(reader, ctypeText, extra);
        if (!res.first || ctypeText.empty()){
            return -1;
        }

        if (!reader.skip_newline_symbol()) {
            return -1;
        }

        if (reader.readable_bytes() < boundary.size()) {
            return -1;
        }

        const size_t from = reader.get_read_offset();
        const int len = reader.find_match_ignore_case(boundary);
        if (len <= 0) {
            return -1;
        }

        if (config::form_data_upload_save) {
            const std::string &tmpName = get_random_filename(originName);
            std::ofstream file(tmpName.c_str(), std::ios_base::out);
            if (!file.good()) {
                return -1;
            }
            extra["____tmp_file_name"] = tmpName;

            if (const auto r = reader.write(file, len); r < 0) {
                std::filesystem::remove(tmpName);
                return -1;
            }

            file.flush();
            file.close();
        }

        result.type_ = ctypeText;
        result.extra_ = extra;
        result.len_ = len;
        result.stream_begin_ = from;

        return 0;
    }

    bool MultipartByterangesParser::can_parse(ContentType contentType)
    {
        return contentType == ContentType::multpart_byte_ranges;
    }

    bool MultipartByterangesParser::parse(HttpPacket *packet)
    {
        auto &reader = packet->get_buffer_reader();
        if (packet->get_body_content() || reader.readable_bytes() == 0) {
            return false;
        }

        size_t begin = reader.get_read_offset();

        const auto &contentExtra = packet->get_content_type_extra();
        const auto it = contentExtra.find("boundary");
        if (it == contentExtra.end()) {
            return false;
        }

        const std::string boundaryStart = "--" + it->second;
        if (reader.readable_bytes() < boundaryStart.size()) {
            return false;
        }

        const std::string boundaryEnd = "--" + it->second + "--";
        auto *rangeContent = new RangeDataContent;
        const auto content = new Content(ContentType::multpart_form_data, rangeContent);

        while (reader.readable_bytes() > 0)
        {
            // skip boundary
            if (!reader.read_match(boundaryStart.c_str(), boundaryStart.size())) {
                delete content;
                return false;
            }

            if (!reader.skip_newline_symbol()) {
                delete content;
                delete rangeContent;
                return false;
            }

            auto *item = new RangeDataItem;
            rangeContent->contents.push_back(item);

            const auto &res = HttpPacket::parse_content_type(reader, item->content_type_.first, item->content_type_.second);
            if (!res.first || item->content_type_.first.empty()){
                delete content;
                delete rangeContent;
                return false;
            }

            if (!reader.skip_newline_symbol()) {
                delete content;
                delete rangeContent;
                return false;
            }

            const auto &rangeRes = parse_content_range(reader);
            if (!std::get<0>(rangeRes)) {
                delete content;
                delete rangeContent;
                return false;
            }

            begin += std::get<4>(rangeRes);

            item->chunk.from = std::get<1>(rangeRes);
            item->chunk.to = std::get<2>(rangeRes);
            item->chunk.length = std::get<3>(rangeRes);
            item->chunk.content.begin = begin;

            if (item->chunk.from > item->chunk.to || item->chunk.length == 0) {
                delete content;
                delete rangeContent;
                return false;
            }

            begin += item->chunk.to - item->chunk.from;
            item->chunk.content.len = begin;

            if (!reader.skip_newline_symbol()) {
                delete content;
                delete rangeContent;
                return false;
            }

            if (!reader.read_match(boundaryEnd.c_str(), boundaryEnd.size())) {
                delete content;
                delete rangeContent;
                return false;
            } else {
                break;
            }
        }

        packet->set_body_content(content);

        return true;
    }

    std::tuple<bool, uint32_t, uint32_t, uint32_t, uint32_t> MultipartByterangesParser::parse_content_range(buffer::BufferReader &reader)
    {
        const size_t fromPos = reader.get_read_offset();
        if (!reader.read_match_ignore_case(http_header_key::content_range)) {
            return {false, 0, 0, 0, 0};
        }

        if (*reader == ':') {
            ++reader;
        }

        if (!reader.skip_newline_symbol()) {
            return {false, 0, 0, 0, 0};
        }

        if (!reader.read_match_ignore_case("bytes")) {
            return {false, 0, 0, 0, 0};
        }

        if (!reader.skip_newline_symbol()) {
            return {false, 0, 0, 0, 0};
        }

        std::string from;
        helper::read_next(reader, '-', from);

        std::string to;
        helper::read_next(reader, '/', to);
        if (to.empty()) {
            return {false, 0, 0, 0, 0};
        }

        std::string sz;
        helper::read_next(reader, '\r', sz);
        if (sz.empty()) {
            return {false, 0, 0, 0, 0};
        }

        return {true, std::atoi(from.c_str()), std::atoi(to.c_str()), std::atoi(sz.c_str()), reader.get_read_offset() - fromPos};
    }
}