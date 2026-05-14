#include "dns_packet.h"
#include <sstream>
#include <cstring>
#include <array>
#include <cstdio>
#include <limits>
#include <vector>

namespace
{
    bool parse_uint16_decimal(const std::string &text, uint16_t &value)
    {
        if (text.empty()) {
            return false;
        }

        uint32_t acc = 0;
        for (char ch : text) {
            if (ch < '0' || ch > '9') {
                return false;
            }
            acc = acc * 10 + static_cast<uint32_t>(ch - '0');
            if (acc > std::numeric_limits<uint16_t>::max()) {
                return false;
            }
        }

        value = static_cast<uint16_t>(acc);
        return true;
    }

    bool parse_ipv4_bytes(const std::string &text, std::array<uint8_t, 4> &out)
    {
        std::size_t start = 0;
        for (std::size_t part = 0; part < out.size(); ++part) {
            const std::size_t dot = text.find('.', start);
            const bool is_last = (part + 1 == out.size());

            if ((dot == std::string::npos) != is_last) {
                return false;
            }

            const std::size_t end = is_last ? text.size() : dot;
            if (end == start) {
                return false;
            }

            uint32_t value = 0;
            for (std::size_t i = start; i < end; ++i) {
                const char ch = text[i];
                if (ch < '0' || ch > '9') {
                    return false;
                }
                value = value * 10 + static_cast<uint32_t>(ch - '0');
                if (value > 255) {
                    return false;
                }
            }

            out[part] = static_cast<uint8_t>(value);
            start = end + 1;
        }

        return true;
    }

    bool parse_ipv6_word(const std::string &token, uint16_t &value)
    {
        if (token.empty() || token.size() > 4) {
            return false;
        }

        uint16_t acc = 0;
        for (char ch : token) {
            uint8_t digit = 0;
            if (ch >= '0' && ch <= '9') {
                digit = static_cast<uint8_t>(ch - '0');
            } else if (ch >= 'a' && ch <= 'f') {
                digit = static_cast<uint8_t>(10 + (ch - 'a'));
            } else if (ch >= 'A' && ch <= 'F') {
                digit = static_cast<uint8_t>(10 + (ch - 'A'));
            } else {
                return false;
            }

            acc = static_cast<uint16_t>((acc << 4) | digit);
        }

        value = acc;
        return true;
    }

    bool parse_ipv6_sequence(const std::string &text, bool allow_ipv4_tail, std::vector<uint16_t> &words)
    {
        if (text.empty()) {
            return true;
        }

        std::size_t start = 0;
        while (start <= text.size()) {
            const std::size_t colon = text.find(':', start);
            const std::size_t end = (colon == std::string::npos) ? text.size() : colon;
            if (end == start) {
                return false;
            }

            const std::string token = text.substr(start, end - start);
            if (token.find('.') != std::string::npos) {
                if (!allow_ipv4_tail || end != text.size()) {
                    return false;
                }

                std::array<uint8_t, 4> ipv4{};
                if (!parse_ipv4_bytes(token, ipv4)) {
                    return false;
                }

                words.push_back(static_cast<uint16_t>((static_cast<uint16_t>(ipv4[0]) << 8) | ipv4[1]));
                words.push_back(static_cast<uint16_t>((static_cast<uint16_t>(ipv4[2]) << 8) | ipv4[3]));
            } else {
                uint16_t word = 0;
                if (!parse_ipv6_word(token, word)) {
                    return false;
                }
                words.push_back(word);
            }

            if (colon == std::string::npos) {
                break;
            }
            start = colon + 1;
        }

        return true;
    }

    bool parse_ipv6_bytes(const std::string &text, std::array<uint8_t, 16> &out)
    {
        if (text.empty()) {
            return false;
        }

        const std::size_t first_double_colon = text.find("::");
        if (first_double_colon == std::string::npos) {
            std::vector<uint16_t> words;
            if (!parse_ipv6_sequence(text, true, words) || words.size() != 8) {
                return false;
            }

            for (std::size_t i = 0; i < words.size(); ++i) {
                out[i * 2] = static_cast<uint8_t>((words[i] >> 8) & 0xFF);
                out[i * 2 + 1] = static_cast<uint8_t>(words[i] & 0xFF);
            }
            return true;
        }

        if (text.find("::", first_double_colon + 2) != std::string::npos) {
            return false;
        }

        const std::string left_text = text.substr(0, first_double_colon);
        const std::string right_text = text.substr(first_double_colon + 2);

        std::vector<uint16_t> left_words;
        std::vector<uint16_t> right_words;
        if (!parse_ipv6_sequence(left_text, false, left_words)) {
            return false;
        }
        if (!parse_ipv6_sequence(right_text, true, right_words)) {
            return false;
        }

        if (left_words.size() + right_words.size() >= 8) {
            return false;
        }

        std::vector<uint16_t> words;
        words.insert(words.end(), left_words.begin(), left_words.end());
        words.resize(8 - right_words.size(), 0);
        words.insert(words.end(), right_words.begin(), right_words.end());

        if (words.size() != 8) {
            return false;
        }

        for (std::size_t i = 0; i < words.size(); ++i) {
            out[i * 2] = static_cast<uint8_t>((words[i] >> 8) & 0xFF);
            out[i * 2 + 1] = static_cast<uint8_t>(words[i] & 0xFF);
        }
        return true;
    }

    std::string format_ipv6_bytes(const std::vector<uint8_t> &rdata)
    {
        if (rdata.size() != 16) {
            return "";
        }

        std::array<uint16_t, 8> words{};
        for (std::size_t i = 0; i < 8; ++i) {
            words[i] = static_cast<uint16_t>((static_cast<uint16_t>(rdata[i * 2]) << 8) | rdata[i * 2 + 1]);
        }

        std::size_t best_start = 0;
        std::size_t best_len = 0;
        for (std::size_t i = 0; i < words.size();) {
            if (words[i] != 0) {
                ++i;
                continue;
            }

            std::size_t j = i;
            while (j < words.size() && words[j] == 0) {
                ++j;
            }

            const std::size_t run_len = j - i;
            if (run_len > best_len) {
                best_start = i;
                best_len = run_len;
            }
            i = j;
        }

        if (best_len < 2) {
            best_len = 0;
        }

        std::string out;
        for (std::size_t i = 0; i < words.size();) {
            if (best_len > 0 && i == best_start) {
                out += out.empty() ? "::" : "::";
                i += best_len;
                continue;
            }

            if (!out.empty() && out.back() != ':') {
                out.push_back(':');
            }

            char word_buf[8] = {0};
            std::snprintf(word_buf, sizeof(word_buf), "%x", words[i]);
            out += word_buf;
            ++i;
        }

        if (out.empty()) {
            return "::";
        }
        return out;
    }

    std::string trim_copy(const std::string &text)
    {
        std::size_t begin = 0;
        while (begin < text.size() && (text[begin] == ' ' || text[begin] == '\t')) {
            ++begin;
        }

        std::size_t end = text.size();
        while (end > begin && (text[end - 1] == ' ' || text[end - 1] == '\t')) {
            --end;
        }

        return text.substr(begin, end - begin);
    }
}

namespace yuan::net::dns
{
    DnsPacket::DnsPacket()
        : session_id_(0)
        , is_response_(false)
        , opcode_(DnsOpcode::QUERY)
        , authoritative_answer_(false)
        , truncated_(false)
        , recursion_desired_(false)
        , recursion_available_(false)
        , response_code_(DnsResponseCode::NO_ERROR)
    {
    }

    DnsPacket::~DnsPacket()
    {
    }

    void DnsPacket::reset()
    {
        session_id_ = 0;
        is_response_ = false;
        opcode_ = DnsOpcode::QUERY;
        authoritative_answer_ = false;
        truncated_ = false;
        recursion_desired_ = false;
        recursion_available_ = false;
        response_code_ = DnsResponseCode::NO_ERROR;
        questions_.clear();
        answers_.clear();
        authorities_.clear();
        additionals_.clear();
    }

    void DnsPacket::encode_name(::yuan::buffer::ByteBuffer &buffer, const std::string &name)
    {
        if (name.empty()) {
            buffer.append_u8(0);
            return;
        }

        size_t start = 0;
        while (start < name.length()) {
            size_t dot_pos = name.find('.', start);
            if (dot_pos == std::string::npos) {
                dot_pos = name.length();
            }

            size_t length = dot_pos - start;
            buffer.append_u8(static_cast<uint8_t>(length));
            buffer.append(name.c_str() + start, length);
            start = dot_pos + 1;
        }

        buffer.append_u8(0);
    }

    bool DnsPacket::decode_name(const uint8_t *data, size_t size, size_t &pos, std::string &name)
    {
        name.clear();
        if (data == nullptr || pos >= size) {
            return false;
        }

        size_t current = pos;
        size_t next_pos = pos;
        bool advanced = false;
        std::vector<bool> visited(size, false);
        size_t jumps = 0;

        while (current < size) {
            if (visited[current]) {
                return false;
            }
            visited[current] = true;

            const uint8_t byte = data[current];

            if (byte == 0) {
                if (!advanced) {
                    next_pos = current + 1;
                    advanced = true;
                }
                break;
            }

            const uint8_t label_type = static_cast<uint8_t>(byte & 0xC0);
            if (label_type == 0xC0) {
                if (current + 1 >= size) {
                    return false;
                }

                const uint16_t offset = static_cast<uint16_t>(((byte & 0x3F) << 8) | data[current + 1]);
                if (offset >= size) {
                    return false;
                }

                if (!advanced) {
                    next_pos = current + 2;
                    advanced = true;
                }

                current = offset;
                ++jumps;
                if (jumps > size) {
                    return false;
                }
                continue;
            }

            if (label_type != 0x00) {
                return false;
            }

            const size_t label_length = byte;
            if (label_length == 0 || label_length > 63) {
                return false;
            }

            const size_t label_start = current + 1;
            if (label_start + label_length > size) {
                return false;
            }

            if (!name.empty()) {
                name.push_back('.');
            }
            name.append(reinterpret_cast<const char*>(data + label_start), label_length);
            current = label_start + label_length;

            if (!advanced) {
                next_pos = current;
            }
        }

        if (!advanced) {
            return false;
        }

        pos = next_pos;
        return true;
    }

    void DnsQuestion::serialize(::yuan::buffer::ByteBuffer &buffer) const
    {
        DnsPacket::encode_name(buffer, name);
        buffer.append_u16(static_cast<uint16_t>(type));
        buffer.append_u16(static_cast<uint16_t>(class_));
    }

    std::pair<bool, DnsQuestion> DnsQuestion::deserialize(::yuan::buffer::ByteBuffer &buffer)
    {
        DnsQuestion question;

        const size_t original_read_offset = buffer.read_offset();
        const char *data = buffer.read_ptr();
        size_t size = buffer.readable_bytes();
        size_t pos = 0;

        if (!DnsPacket::decode_name(reinterpret_cast<const uint8_t*>(data), size, pos, question.name)) {
            return {false, question};
        }

        if (pos + 4 > size) {
            return {false, question};
        }

        question.type = static_cast<DnsType>(ntohs(*reinterpret_cast<const uint16_t*>(data + pos)));
        pos += 2;
        question.class_ = static_cast<DnsClass>(ntohs(*reinterpret_cast<const uint16_t*>(data + pos)));
        pos += 2;

        buffer.set_read_offset(original_read_offset + pos);
        return {true, question};
    }

    void DnsResourceRecord::serialize(::yuan::buffer::ByteBuffer &buffer) const
    {
        DnsPacket::encode_name(buffer, name);
        buffer.append_u16(static_cast<uint16_t>(type));
        buffer.append_u16(static_cast<uint16_t>(class_));
        buffer.append_u32(ttl);
        buffer.append_u16(static_cast<uint16_t>(rdata.size()));
        buffer.append(rdata.data(), rdata.size());
    }

    std::pair<bool, DnsResourceRecord> DnsResourceRecord::deserialize(::yuan::buffer::ByteBuffer &buffer)
    {
        DnsResourceRecord record;

        const size_t original_read_offset = buffer.read_offset();
        const char *data = buffer.read_ptr();
        size_t size = buffer.readable_bytes();
        size_t pos = 0;

        if (!DnsPacket::decode_name(reinterpret_cast<const uint8_t*>(data), size, pos, record.name)) {
            return {false, record};
        }

        if (pos + 10 > size) {
            return {false, record};
        }

        record.type = static_cast<DnsType>(ntohs(*reinterpret_cast<const uint16_t*>(data + pos)));
        pos += 2;
        record.class_ = static_cast<DnsClass>(ntohs(*reinterpret_cast<const uint16_t*>(data + pos)));
        pos += 2;
        record.ttl = ntohl(*reinterpret_cast<const uint32_t*>(data + pos));
        pos += 4;
        uint16_t rdlength = ntohs(*reinterpret_cast<const uint16_t*>(data + pos));
        pos += 2;

        if (pos + rdlength > size) {
            return {false, record};
        }

        record.rdata.assign(reinterpret_cast<const uint8_t*>(data + pos),
                          reinterpret_cast<const uint8_t*>(data + pos + rdlength));
        pos += rdlength;

        buffer.set_read_offset(original_read_offset + pos);
        return {true, record};
    }

    std::string DnsResourceRecord::get_rdata_as_string() const
    {
        if (rdata.empty()) {
            return "";
        }

        switch (type) {
            case DnsType::A: {
                if (rdata.size() != 4) return "";
                char ip[16];
                snprintf(ip, sizeof(ip), "%d.%d.%d.%d",
                         rdata[0], rdata[1], rdata[2], rdata[3]);
                return std::string(ip);
            }
            case DnsType::AAAA: {
                return format_ipv6_bytes(rdata);
            }
            case DnsType::TXT: {
                if (rdata.empty()) return "";
                // TXT record format: <length><data>
                size_t pos = 0;
                std::string result;
                while (pos < rdata.size()) {
                    if (pos + 1 > rdata.size()) break;
                    uint8_t len = rdata[pos];
                    pos++;
                    if (pos + len > rdata.size()) break;
                    result += std::string(reinterpret_cast<const char*>(rdata.data() + pos), len);
                    pos += len;
                    if (pos < rdata.size()) result += " ";
                }
                return result;
            }
            case DnsType::CNAME:
            case DnsType::NS:
            case DnsType::MX: {
                size_t pos = 0;
                if (rdata.size() < 3) {
                    return "";
                }

                const uint16_t preference = static_cast<uint16_t>((static_cast<uint16_t>(rdata[0]) << 8) | rdata[1]);
                pos = 2;
                std::string exchange;
                if (!DnsPacket::decode_name(rdata.data(), rdata.size(), pos, exchange) || exchange.empty()) {
                    return "";
                }
                if (pos != rdata.size()) {
                    return "";
                }
                return std::to_string(preference) + " " + exchange;
            }
            default:
                return "";
        }
    }

    void DnsResourceRecord::set_rdata_from_string(const std::string &data)
    {
        switch (type) {
            case DnsType::A: {
                std::array<uint8_t, 4> ipv4{};
                if (parse_ipv4_bytes(data, ipv4)) {
                    rdata.assign(ipv4.begin(), ipv4.end());
                }
                break;
            }
            case DnsType::AAAA: {
                std::array<uint8_t, 16> ipv6{};
                if (parse_ipv6_bytes(data, ipv6)) {
                    rdata.assign(ipv6.begin(), ipv6.end());
                }
                break;
            }
            case DnsType::TXT: {
                // TXT record: store as <length><data>
                rdata.clear();
                if (!data.empty()) {
                    // Split by spaces for multiple TXT strings
                    size_t start = 0;
                    while (start < data.length()) {
                        size_t space_pos = data.find(' ', start);
                        if (space_pos == std::string::npos) space_pos = data.length();
                        
                        std::string part = data.substr(start, space_pos - start);
                        if (part.length() > 255) part = part.substr(0, 255); // Max 255 chars per TXT chunk
                        
                        rdata.push_back(static_cast<uint8_t>(part.length()));
                        rdata.insert(rdata.end(), part.begin(), part.end());
                        
                        start = space_pos + 1;
                    }
                }
                break;
            }
            case DnsType::CNAME:
            case DnsType::NS: {
                rdata.clear();
                ::yuan::buffer::ByteBuffer name_buf;
                DnsPacket::encode_name(name_buf, data);
                const auto span = name_buf.readable_span();
                rdata.insert(rdata.end(),
                             reinterpret_cast<const uint8_t *>(span.data()),
                             reinterpret_cast<const uint8_t *>(span.data() + span.size()));
                break;
            }
            case DnsType::MX: {
                rdata.clear();

                const std::string mx_text = trim_copy(data);
                if (mx_text.empty()) {
                    break;
                }

                uint16_t preference = 10;
                std::string exchange = mx_text;

                const std::size_t split = mx_text.find_first_of(" \t");
                if (split != std::string::npos) {
                    const std::string pref_text = mx_text.substr(0, split);
                    std::string rest = trim_copy(mx_text.substr(split + 1));
                    uint16_t parsed_pref = 0;
                    if (!rest.empty() && parse_uint16_decimal(pref_text, parsed_pref)) {
                        preference = parsed_pref;
                        exchange = rest;
                    }
                }

                if (exchange.empty()) {
                    break;
                }

                ::yuan::buffer::ByteBuffer exchange_buf;
                DnsPacket::encode_name(exchange_buf, exchange);
                const auto span = exchange_buf.readable_span();

                rdata.push_back(static_cast<uint8_t>((preference >> 8) & 0xFF));
                rdata.push_back(static_cast<uint8_t>(preference & 0xFF));
                rdata.insert(rdata.end(),
                             reinterpret_cast<const uint8_t *>(span.data()),
                             reinterpret_cast<const uint8_t *>(span.data() + span.size()));
                break;
            }
            default:
                rdata.assign(data.begin(), data.end());
        }
    }

    bool DnsPacket::serialize(::yuan::buffer::ByteBuffer &buffer) const
    {
        // Write header
        buffer.append_u16(session_id_);

        uint16_t flags = 0;
        if (is_response_) flags |= 0x8000;
        flags |= (static_cast<uint8_t>(opcode_) & 0x0F) << 11;
        if (authoritative_answer_) flags |= 0x0400;
        if (truncated_) flags |= 0x0200;
        if (recursion_desired_) flags |= 0x0100;
        if (recursion_available_) flags |= 0x0080;
        flags |= static_cast<uint8_t>(response_code_) & 0x0F;
        buffer.append_u16(flags);

        buffer.append_u16(static_cast<uint16_t>(questions_.size()));
        buffer.append_u16(static_cast<uint16_t>(answers_.size()));
        buffer.append_u16(static_cast<uint16_t>(authorities_.size()));
        buffer.append_u16(static_cast<uint16_t>(additionals_.size()));

        // Write questions
        for (const auto &question : questions_) {
            question.serialize(buffer);
        }

        // Write resource records
        for (const auto &record : answers_) {
            record.serialize(buffer);
        }

        for (const auto &record : authorities_) {
            record.serialize(buffer);
        }

        for (const auto &record : additionals_) {
            record.serialize(buffer);
        }

        return true;
    }

    bool DnsPacket::deserialize(::yuan::buffer::ByteBuffer &buffer)
    {
        const size_t original_read_offset = buffer.read_offset();
        const char *data = buffer.read_ptr();
        size_t size = buffer.readable_bytes();

        if (size < 12) {
            return false;
        }

        size_t pos = 0;

        // Read header
        session_id_ = ntohs(*reinterpret_cast<const uint16_t*>(data + pos));
        pos += 2;

        uint16_t flags = ntohs(*reinterpret_cast<const uint16_t*>(data + pos));
        pos += 2;
        is_response_ = (flags & 0x8000) != 0;
        opcode_ = static_cast<DnsOpcode>((flags >> 11) & 0x0F);
        authoritative_answer_ = (flags & 0x0400) != 0;
        truncated_ = (flags & 0x0200) != 0;
        recursion_desired_ = (flags & 0x0100) != 0;
        recursion_available_ = (flags & 0x0080) != 0;
        response_code_ = static_cast<DnsResponseCode>(flags & 0x0F);

        uint16_t qdcount = ntohs(*reinterpret_cast<const uint16_t*>(data + pos));
        pos += 2;
        uint16_t ancount = ntohs(*reinterpret_cast<const uint16_t*>(data + pos));
        pos += 2;
        uint16_t nscount = ntohs(*reinterpret_cast<const uint16_t*>(data + pos));
        pos += 2;
        uint16_t arcount = ntohs(*reinterpret_cast<const uint16_t*>(data + pos));
        pos += 2;

        // Update buffer read index to after header
        buffer.set_read_offset(original_read_offset + pos);

        // Read questions
        questions_.clear();
        for (uint16_t i = 0; i < qdcount; ++i) {
            auto result = DnsQuestion::deserialize(buffer);
            if (!result.first) {
                return false;
            }
            questions_.push_back(result.second);
        }

        // Read answers
        answers_.clear();
        for (uint16_t i = 0; i < ancount; ++i) {
            auto result = DnsResourceRecord::deserialize(buffer);
            if (!result.first) {
                return false;
            }
            answers_.push_back(result.second);
        }

        // Read authorities
        authorities_.clear();
        for (uint16_t i = 0; i < nscount; ++i) {
            auto result = DnsResourceRecord::deserialize(buffer);
            if (!result.first) {
                return false;
            }
            authorities_.push_back(result.second);
        }

        // Read additionals
        additionals_.clear();
        for (uint16_t i = 0; i < arcount; ++i) {
            auto result = DnsResourceRecord::deserialize(buffer);
            if (!result.first) {
                return false;
            }
            additionals_.push_back(result.second);
        }

        return true;
    }

    std::string DnsPacket::to_string() const
    {
        std::stringstream ss;
        ss << "DNS Packet:\n";
        ss << "  Session ID: " << session_id_ << "\n";
        ss << "  " << (is_response_ ? "Response" : "Query") << "\n";
        ss << "  Opcode: " << static_cast<int>(opcode_) << "\n";
        ss << "  Response Code: " << static_cast<int>(response_code_) << "\n";
        ss << "  Questions: " << questions_.size() << "\n";
        ss << "  Answers: " << answers_.size() << "\n";
        ss << "  Authorities: " << authorities_.size() << "\n";
        ss << "  Additionals: " << additionals_.size() << "\n";

        for (size_t i = 0; i < questions_.size(); ++i) {
            ss << "  Question " << i << ": " << questions_[i].name << "\n";
        }

        for (size_t i = 0; i < answers_.size(); ++i) {
            ss << "  Answer " << i << ": " << answers_[i].name
               << " -> " << answers_[i].get_rdata_as_string() << "\n";
        }

        return ss.str();
    }
}
