#include "content/parsers/multipart_content_parser.h"
#include "base/time.h"
#include "header_util.h"
#include "base/utils/base64.h"
#include "content/types.h"
#include "content_type.h"
#include "packet.h"
#include "header_key.h"
#include "ops/option.h"

#include <cstdlib>
#include <fstream>
#include <memory>

namespace yuan::net::http 
{

// ============================================================
// Multipart Form Data Parser
// ============================================================

bool MultipartFormDataParser::can_parse(ContentType contentType)
{
    return contentType == ContentType::multpart_form_data;
}

bool MultipartFormDataParser::parse(HttpPacket *packet)
{
    const char *body_begin = packet->body_begin();
    const char *body_end   = packet->body_end();
    if (!body_begin || !body_end || body_begin >= body_end) return false;

    const auto &ct_extra = packet->get_content_type_extra();
    auto boundary_it = ct_extra.find("boundary");
    if (boundary_it == ct_extra.end()) return false;
    const std::string &boundary = boundary_it->second;
    if (boundary.empty()) return false;

    const std::string delim_start = "--" + boundary;
    const std::string delim_end   = "--" + boundary + "--";

    auto form_data = std::make_unique<FormDataContent>();
    const char *pos = body_begin;

    while (pos < body_end)
    {
        // 跳过 part 之间的 \r\n（第1个 part 前没有，后续 part 前有 \r\n--boundary）
        {
            uint32_t nl = helper::skip_new_line(pos);
            if (nl > 0) pos += nl;
        }

        // check boundary start
        if ((body_end - pos) < static_cast<ptrdiff_t>(delim_start.size())) break;
        if (pos[0] != '-' || pos[1] != '-') { 
            break; 
        }
        
        pos += delim_start.size();
        pos += helper::skip_new_line(pos);
        if (pos > body_end) break;

        // parse Content-Disposition
        auto dis = parse_content_disposition(pos, body_end);
        if (dis.first == 0) break;

        pos += dis.first;
        pos += helper::skip_new_line(pos);

        auto name_it = dis.second.second.find(helper::name_);
        if (name_it == dis.second.second.end()) break;
        const std::string &field_name = name_it->second;
        auto filename_it = dis.second.second.find(helper::filename_);

        pos += helper::skip_new_line(pos);  // skip possible empty line or content-type line

        if (filename_it == dis.second.second.end())
        {
            // text field
            auto [consumed, value] = parse_text_part(pos, body_end);
            if (!value.empty()) {
                form_data->fields[field_name] =
                    std::make_shared<FormDataStringItem>(std::move(value));
            }
            pos += consumed;
        }
        else
        {
            // file field
            FilePartResult file_result;
            int ret = parse_file_part(file_result, packet, pos, body_end, filename_it->second);
            if (ret < 0) break;

            form_data->fields[field_name] = std::make_shared<FormDataFileItem>(
                filename_it->second,
                file_result.content_type,
                file_result.data_begin,
                file_result.data_end,
                std::move(file_result.tmp_file_path)
            );
            
            pos += file_result.parsed_bytes;
        }

        pos += helper::skip_new_line(pos);
        
        // check end boundary
        if ((body_end - pos) >= static_cast<ptrdiff_t>(delim_end.size()) &&
            helper::str_cmp(pos, pos + delim_end.size(), delim_end.c_str()))
        {
            break;  // done
        }
    }

    packet->set_body_content(new Content(ContentType::multpart_form_data, form_data.release()));
    return true;
}

// ============================================================
// Parse: Content-Disposition: form-data; name="x"; filename="y"
// ============================================================

MultipartFormDataParser::DispositionResult 
MultipartFormDataParser::parse_content_disposition(const char *begin, const char *end)
{
    DispositionResult result = {0, {}};
    const char *p = begin;

    constexpr size_t prefix_len = 20;  // "content-disposition:"
    if (end - p < static_cast<ptrdiff_t>(prefix_len)) return result;
    if (!helper::str_cmp(p, p + prefix_len, "content-disposition:")) return result;
    p += prefix_len;

    std::string type;
    for (; p < end; ++p) {
        char ch = *p;
        if (ch == ' ') continue;
        if (ch == ';') { ++p; break; }
        type.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    }
    result.second.first = std::move(type);

    auto &params = result.second.second;
    while (p < end) {
        const auto &key = helper::read_identifier(p, end);
        if (key.empty()) break;
        p += key.size() + 2;  // skip '='
        const auto &val = helper::read_identifier(p, end);
        if (val.empty()) break;
        params[key] = val;
        p += val.size() + 2;
        if (p < end && *p == ';') { ++p; } else { break; }
    }

    if (params.empty()) { result.first = 0; return result; }
    result.first = static_cast<uint32_t>(p - begin);
    return result;
}

// ============================================================
// Parse text field value (until \r\n or boundary)
// ============================================================

std::pair<uint32_t, std::string> 
MultipartFormDataParser::parse_text_part(const char *begin, const char *end)
{
    const char *p = begin;
    while (p < end && *p != '\r' && *p != '\n') ++p;
    return {static_cast<uint32_t>(p - begin), std::string(begin, p)};
}

// ============================================================
// Generate random temp filename
// ============================================================

namespace {
    inline std::string gen_tmp_filename(const std::string &origin)
    {
        std::string ext;
        auto dot = origin.rfind('.');
        if (dot != std::string::npos) ext = origin.substr(dot);
        auto now = static_cast<time_t>(yuan::base::time::system_now_seconds());
        unsigned int rnd = static_cast<unsigned int>(rand());
        auto b64 = base::util::base64_encode(origin);
        return "upload_" + std::to_string(now) + "_" +
               std::to_string(rnd) + "_" + b64.substr(0, 8) + ext;
    }
}

// ============================================================
// Parse file part content
// ============================================================

int MultipartFormDataParser::parse_file_part(FilePartResult &result, HttpPacket *packet,
                                              const char *begin, const char *end,
                                              const std::string &origin_filename)
{
    const char *start_pos = begin;
    const char *p = begin;

    // read "Content-Type:" header line
    std::string ctype_header;
    while (p < end && *p != ':') {
        ctype_header.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(*p))));
        ++p;
    }
    if (ctype_header != http_header_key::content_type) return -1;
    ++p;
    if (p < end && *p == ' ') ++p;

    // parse content-type value and params
    std::string ctype_text;
    auto [ok, consumed] = packet->parse_content_type(p, end, ctype_text, result.content_type_params);
    if (!ok || ctype_text.empty()) return -1;

    p += consumed;
    p += helper::skip_new_line(p);  // skip blank line after headers

    // find boundary to locate data end
    const auto &ct_extra = packet->get_content_type_extra();
    auto bnd_it = ct_extra.find("boundary");
    if (bnd_it == ct_extra.end()) return -1;

    const std::string delim = "--" + bnd_it->second;
    const char *data_start = p;

    // scan for boundary to locate data end
    while (p <= end - static_cast<ptrdiff_t>(delim.size())) {
        if (helper::str_cmp(p, p + delim.size(), delim.c_str()))
            break;
        ++p;
    }

    // data ends right before \r\n--boundary (or at boundary if no \r\n)
    const char *data_end = p;
    // strip trailing \r\n
    if (data_end > data_start + 1 && *(data_end - 1) == '\n') --data_end;
    if (data_end > data_start && *(data_end - 1) == '\r') --data_end;

    result.data_begin = data_start;
    result.data_end   = data_end;
    // parsed_bytes 只到边界开始处（不含边界本身），让主循环的 -- 检查能匹配到
    result.parsed_bytes = static_cast<uint32_t>(p - start_pos);
    result.content_type = ctype_text;

    // optionally persist to disk
    if (config::form_data_upload_save && data_end > data_start)
    {
        const std::string tmp_path = gen_tmp_filename(origin_filename);
        std::ofstream ofs(tmp_path, std::ios::binary);
        if (ofs.good())
        {
            ofs.write(data_start, data_end - data_start);
            ofs.flush();
            ofs.close();
            result.tmp_file_path = tmp_path;
            result.data_begin = nullptr;
            result.data_end   = nullptr;
        }
    }

    return 0;
}


// ============================================================
// Multipart Byte Ranges Parser (low usage, kept for compat)
// ============================================================

bool MultipartByterangesParser::can_parse(ContentType contentType)
{
    return contentType == ContentType::multpart_byte_ranges;
}

bool MultipartByterangesParser::parse(HttpPacket *packet)
{
    if (packet->get_body_content()) return false;
    
    const char *begin = packet->body_begin();
    const char *end   = packet->body_end();
    if (!begin || !end || begin >= end) return false;

    constexpr std::size_t MAX_RANGE_DATA_SIZE = 100 * 1024 * 1024; // 单次 range 响应上限 100MB

    const auto &extra = packet->get_content_type_extra();
    auto it = extra.find("boundary");
    if (it == extra.end()) return false;

    const std::string ds = "--" + it->second;
    const std::string de = "--" + it->second + "--";

    auto range_content = std::make_unique<RangeDataContent>();
    const char *pos = begin;

    while (pos < end)
    {
        if ((end - pos) < static_cast<ptrdiff_t>(ds.size())) break;
        if (!helper::str_cmp(pos, pos + ds.size(), ds.c_str())) break;

        pos += ds.size();
        pos += helper::skip_new_line(pos);

        auto chunk = RangeChunk{};

        // skip content-type line of this range part
        while (pos < end && (*pos != '\r' && *pos != '\n')) ++pos;
        pos += helper::skip_new_line(pos);

        // parse Content-Range header
        auto [range_ok, from, to, length, consumed] = parse_content_range(pos, end);
        if (!range_ok) break;
        
        chunk.from   = from;
        chunk.to     = to;
        chunk.length = length;
        pos += consumed;

        // locate data: scan for next boundary ("--")
        const char *dbegin = pos;
        while (pos < end - 1) {
            if (pos[0] == '-' && pos[1] == '-') break;
            ++pos;
        }

        const char *dend = pos;
        if (dend > dbegin + 1 && *(dend-1) == '\n') --dend;
        if (dend > dbegin && *(dend-1) == '\r') --dend;

        // 安全检查: 防止单个 chunk 过大导致 OOM
        std::size_t data_size = static_cast<std::size_t>(dend > dbegin ? dend - dbegin : 0);
        if (data_size > MAX_RANGE_DATA_SIZE) return false;

        chunk.data.assign(dbegin, dend);
        range_content->chunks.push_back(std::move(chunk));

        pos += helper::skip_new_line(pos);
        if ((end - pos) >= static_cast<ptrdiff_t>(de.size()) &&
            helper::str_cmp(pos, pos + de.size(), de.c_str()))
        {
            break;
        }
    }

    packet->set_body_content(new Content(ContentType::multpart_byte_ranges, range_content.release()));
    return true;
}

MultipartByterangesParser::RangeParseResult 
MultipartByterangesParser::parse_content_range(const char *begin, const char *end)
{
    const char *p = begin;
    if (end - p < 13 || !helper::str_cmp(p, p + 13, http_header_key::content_range))
        return {false, 0, 0, 0, 0};

    p += 13;
    if (*p == ':') ++p;
    p += helper::skip_new_line(p);

    if (end - p < 5 || !helper::str_cmp(p, p + 5, "bytes"))
        return {false, 0, 0, 0, 0};
    p += 5;
    // 跳过 bytes 后的空格
    if (p < end && *p == ' ') ++p;

    std::string s_from, s_to, s_sz;
    helper::read_next(p, end, '-', s_from);
    p += s_from.size() + (s_from.empty() ? 0 : 1);

    helper::read_next(p, end, '/', s_to);
    if (s_to.empty()) return {false, 0, 0, 0, 0};
    p += s_to.size() + 1;

    helper::read_next(p, end, '\r', s_sz);
    if (s_sz.empty()) return {false, 0, 0, 0, 0};
    p += s_sz.size() + 2; // skip \r\n

    // 使用 stoul 替代 atoi，避免溢出截断
    auto safe_stoul = [](const std::string &s) -> uint32_t {
        unsigned long v = std::stoul(s, nullptr, 10);
        return v > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(v);
    };

    return {true,
            safe_stoul(s_from),
            safe_stoul(s_to),
            safe_stoul(s_sz),
            static_cast<uint32_t>(p - begin)};
}

} // namespace yuan::net::http
