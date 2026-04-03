#include "dns_packet.h"
#include <sstream>
#include <cstring>
#include <iostream>

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

    void DnsPacket::encode_name(::yuan::buffer::Buffer &buffer, const std::string &name)
    {
        if (name.empty()) {
            buffer.write_uint8(0);
            return;
        }

        size_t start = 0;
        while (start < name.length()) {
            size_t dot_pos = name.find('.', start);
            if (dot_pos == std::string::npos) {
                dot_pos = name.length();
            }

            size_t length = dot_pos - start;
            buffer.write_uint8(static_cast<uint8_t>(length));
            buffer.write_string(name.c_str() + start, length);
            start = dot_pos + 1;
        }

        buffer.write_uint8(0);
    }

    std::string DnsPacket::decode_name(const uint8_t *data, size_t size, size_t &pos)
    {
        std::string result;
        size_t original_pos = pos;
        bool jumped = false;

        while (pos < size) {
            uint8_t byte = data[pos];

            if (byte == 0) {
                pos++;
                break;
            }

            // Check for compression pointer (top 2 bits are 11)
            if ((byte & 0xC0) == 0xC0) {
                if (pos + 1 >= size) {
                    return "";
                }

                uint16_t offset = ((byte & 0x3F) << 8) | data[pos + 1];
                pos += 2;

                if (!jumped) {
                    original_pos = pos;
                    jumped = true;
                }
                pos = offset;
                continue;
            }

            // Regular label
            pos++;
            if (byte + pos > size) {
                return "";
            }

            if (!result.empty()) {
                result += '.';
            }
            result.append(reinterpret_cast<const char*>(data + pos), byte);
            pos += byte;
        }

        if (jumped) {
            pos = original_pos;
        }

        return result;
    }

    void DnsQuestion::serialize(::yuan::buffer::Buffer &buffer) const
    {
        DnsPacket::encode_name(buffer, name);
        buffer.write_uint16(static_cast<uint16_t>(type));
        buffer.write_uint16(static_cast<uint16_t>(class_));
    }

    std::pair<bool, DnsQuestion> DnsQuestion::deserialize(::yuan::buffer::Buffer &buffer)
    {
        DnsQuestion question;

        size_t original_read_index = buffer.get_read_index();
        const char *data = buffer.peek();
        size_t size = buffer.readable_bytes();
        size_t pos = 0;

        question.name = DnsPacket::decode_name(reinterpret_cast<const uint8_t*>(data), size, pos);
        if (pos >= size || question.name.empty()) {
            return {false, question};
        }

        if (pos + 4 > size) {
            return {false, question};
        }

        question.type = static_cast<DnsType>(ntohs(*reinterpret_cast<const uint16_t*>(data + pos)));
        pos += 2;
        question.class_ = static_cast<DnsClass>(ntohs(*reinterpret_cast<const uint16_t*>(data + pos)));
        pos += 2;

        buffer.set_read_index(original_read_index + pos);
        return {true, question};
    }

    void DnsResourceRecord::serialize(::yuan::buffer::Buffer &buffer) const
    {
        DnsPacket::encode_name(buffer, name);
        buffer.write_uint16(static_cast<uint16_t>(type));
        buffer.write_uint16(static_cast<uint16_t>(class_));
        buffer.write_uint32(ttl);
        buffer.write_uint16(static_cast<uint16_t>(rdata.size()));
        buffer.write_string(reinterpret_cast<const char*>(rdata.data()), rdata.size());
    }

    std::pair<bool, DnsResourceRecord> DnsResourceRecord::deserialize(::yuan::buffer::Buffer &buffer)
    {
        DnsResourceRecord record;

        size_t original_read_index = buffer.get_read_index();
        const char *data = buffer.peek();
        size_t size = buffer.readable_bytes();
        size_t pos = 0;

        record.name = DnsPacket::decode_name(reinterpret_cast<const uint8_t*>(data), size, pos);
        if (pos >= size || record.name.empty()) {
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

        buffer.set_read_index(original_read_index + pos);
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
                if (rdata.size() != 16) return "";
                char ip[64];
                snprintf(ip, sizeof(ip), "%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x",
                         rdata[0], rdata[1], rdata[2], rdata[3], rdata[4], rdata[5],
                         rdata[6], rdata[7], rdata[8], rdata[9], rdata[10], rdata[11],
                         rdata[12], rdata[13], rdata[14], rdata[15]);
                return std::string(ip);
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
                // These are domain names - decode them
                size_t pos = 0;
                return DnsPacket::decode_name(rdata.data(), rdata.size(), pos);
            }
            default:
                return "";
        }
    }

    void DnsResourceRecord::set_rdata_from_string(const std::string &data)
    {
        switch (type) {
            case DnsType::A: {
                int a, b, c, d;
                if (sscanf(data.c_str(), "%d.%d.%d.%d", &a, &b, &c, &d) == 4) {
                    rdata.resize(4);
                    rdata[0] = static_cast<uint8_t>(a);
                    rdata[1] = static_cast<uint8_t>(b);
                    rdata[2] = static_cast<uint8_t>(c);
                    rdata[3] = static_cast<uint8_t>(d);
                }
                break;
            }
            case DnsType::AAAA: {
                // Simple IPv6 parsing (doesn't handle compression ::)
                unsigned int parts[16] = {0};
                int parsed = sscanf(data.c_str(), "%2x%2x:%2x%2x:%2x%2x:%2x%2x:%2x%2x:%2x%2x:%2x%2x:%2x%2x",
                                 &parts[0], &parts[1], &parts[2], &parts[3],
                                 &parts[4], &parts[5], &parts[6], &parts[7],
                                 &parts[8], &parts[9], &parts[10], &parts[11],
                                 &parts[12], &parts[13], &parts[14], &parts[15]);
                rdata.resize(16);
                for (int i = 0; i < 16; i++) {
                    rdata[i] = static_cast<uint8_t>(parts[i]);
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
            case DnsType::NS:
            case DnsType::MX: {
                // Encode domain name
                rdata.clear();
                size_t start = 0;
                while (start < data.length()) {
                    size_t dot_pos = data.find('.', start);
                    if (dot_pos == std::string::npos) dot_pos = data.length();
                    
                    size_t length = dot_pos - start;
                    if (length > 63) length = 63; // Max label length
                    rdata.push_back(static_cast<uint8_t>(length));
                    rdata.insert(rdata.end(), data.begin() + start, data.begin() + dot_pos);
                    start = dot_pos + 1;
                }
                rdata.push_back(0); // Null terminator
                break;
            }
            default:
                rdata.assign(data.begin(), data.end());
        }
    }

    bool DnsPacket::serialize(::yuan::buffer::Buffer &buffer) const
    {
        // Write header
        buffer.write_uint16(session_id_);

        uint16_t flags = 0;
        if (is_response_) flags |= 0x8000;
        flags |= (static_cast<uint8_t>(opcode_) & 0x0F) << 11;
        if (authoritative_answer_) flags |= 0x0400;
        if (truncated_) flags |= 0x0200;
        if (recursion_desired_) flags |= 0x0100;
        if (recursion_available_) flags |= 0x0080;
        flags |= static_cast<uint8_t>(response_code_) & 0x0F;
        buffer.write_uint16(flags);

        buffer.write_uint16(static_cast<uint16_t>(questions_.size()));
        buffer.write_uint16(static_cast<uint16_t>(answers_.size()));
        buffer.write_uint16(static_cast<uint16_t>(authorities_.size()));
        buffer.write_uint16(static_cast<uint16_t>(additionals_.size()));

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

    bool DnsPacket::deserialize(::yuan::buffer::Buffer &buffer)
    {
        size_t original_read_index = buffer.get_read_index();
        const char *data = buffer.peek();
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
        buffer.set_read_index(original_read_index + pos);

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
