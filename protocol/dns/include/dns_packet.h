#ifndef __NET_DNS_PACKET_H__
#define __NET_DNS_PACKET_H__
#include <cstdint>
#include <string>
#include <vector>
#include "buffer/buffer.h"

#ifdef IN
#undef IN
#endif

#ifdef NO_ERROR
#undef NO_ERROR
#endif

namespace yuan::net::dns
{
    enum class DnsType : uint16_t
    {
        A = 1,
        NS = 2,
        CNAME = 5,
        SOA = 6,
        MX = 15,
        TXT = 16,
        AAAA = 28,
        ANY = 255
    };

    enum class DnsClass : uint16_t
    {
        IN = 1
    };

    enum class DnsOpcode : uint8_t
    {
        QUERY = 0,
        IQUERY = 1,
        STATUS = 2
    };

    enum class DnsResponseCode : uint8_t
    {
        NO_ERROR = 0,
        FORMAT_ERROR = 1,
        SERVER_FAILURE = 2,
        NAME_ERROR = 3,
        NOT_IMPLEMENTED = 4,
        REFUSED = 5
    };

    struct DnsQuestion
    {
        std::string name;
        DnsType type;
        DnsClass class_;

        void serialize(::yuan::buffer::Buffer &buffer) const;
        static std::pair<bool, DnsQuestion> deserialize(::yuan::buffer::Buffer &buffer);
    };

    struct DnsResourceRecord
    {
        std::string name;
        DnsType type;
        DnsClass class_;
        uint32_t ttl;
        std::vector<uint8_t> rdata;

        void serialize(::yuan::buffer::Buffer &buffer) const;
        static std::pair<bool, DnsResourceRecord> deserialize(::yuan::buffer::Buffer &buffer);

        std::string get_rdata_as_string() const;
        void set_rdata_from_string(const std::string &data);
    };

    class DnsPacket
    {
    public:
        DnsPacket();
        ~DnsPacket();

        bool serialize(::yuan::buffer::Buffer &buffer) const;
        bool deserialize(::yuan::buffer::Buffer &buffer);
        void reset();

        uint16_t get_session_id() const { return session_id_; }
        void set_session_id(uint16_t id) { session_id_ = id; }

        bool is_response() const { return is_response_; }
        void set_is_response(bool is_response) { is_response_ = is_response; }

        DnsOpcode get_opcode() const { return opcode_; }
        void set_opcode(DnsOpcode opcode) { opcode_ = opcode; }

        bool is_authoritative_answer() const { return authoritative_answer_; }
        void set_authoritative_answer(bool flag) { authoritative_answer_ = flag; }

        bool is_truncated() const { return truncated_; }
        void set_truncated(bool flag) { truncated_ = flag; }

        bool is_recursion_desired() const { return recursion_desired_; }
        void set_recursion_desired(bool flag) { recursion_desired_ = flag; }

        bool is_recursion_available() const { return recursion_available_; }
        void set_recursion_available(bool flag) { recursion_available_ = flag; }

        DnsResponseCode get_response_code() const { return response_code_; }
        void set_response_code(DnsResponseCode code) { response_code_ = code; }

        const std::vector<DnsQuestion>& get_questions() const { return questions_; }
        void add_question(const DnsQuestion &question) { questions_.push_back(question); }

        const std::vector<DnsResourceRecord>& get_answers() const { return answers_; }
        void add_answer(const DnsResourceRecord &record) { answers_.push_back(record); }

        const std::vector<DnsResourceRecord>& get_authorities() const { return authorities_; }
        void add_authority(const DnsResourceRecord &record) { authorities_.push_back(record); }

        const std::vector<DnsResourceRecord>& get_additionals() const { return additionals_; }
        void add_additional(const DnsResourceRecord &record) { additionals_.push_back(record); }

        std::string to_string() const;

        static void encode_name(::yuan::buffer::Buffer &buffer, const std::string &name);
        static std::string decode_name(const uint8_t *data, size_t size, size_t &pos);

    private:
        uint16_t session_id_;
        bool is_response_;
        DnsOpcode opcode_;
        bool authoritative_answer_;
        bool truncated_;
        bool recursion_desired_;
        bool recursion_available_;
        DnsResponseCode response_code_;

        std::vector<DnsQuestion> questions_;
        std::vector<DnsResourceRecord> answers_;
        std::vector<DnsResourceRecord> authorities_;
        std::vector<DnsResourceRecord> additionals_;
    };
}

#endif