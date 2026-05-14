#include "dns_server.h"
#include "dns_packet.h"
#include "buffer/byte_buffer.h"
#include "logger.h"
#include "net/session/connection_context.h"
#include <algorithm>

namespace yuan::net::dns
{
    DnsServer::DnsServer()
        : port_(53), running_(false)
    {
        add_record("localhost", "127.0.0.1");
        session_.set_read_callback([this](ConnectionContext &ctx) {
            handle_dns_query(ctx);
        });
    }

    DnsServer::~DnsServer()
    {
        stop();
    }

    void DnsServer::handle_dns_query(ConnectionContext & ctx)
    {
        auto byte_buffer = ctx.take_input_byte_buffer();
        if (byte_buffer.readable_bytes() == 0) {
            return;
        }

        DnsPacket query;
        if (!query.deserialize(byte_buffer)) {
            LOG_WARN("Failed to parse DNS query");
            return;
        }

        DnsPacket response;
        create_response(query, response);

        yuan::buffer::ByteBuffer response_buffer;
        response.serialize(response_buffer);
        ctx.write_and_flush(response_buffer);
    }

    void DnsServer::create_response(const DnsPacket & query, DnsPacket & response)
    {
        response.set_session_id(query.get_session_id());
        response.set_is_response(true);
        response.set_opcode(query.get_opcode());
        response.set_recursion_desired(query.is_recursion_desired());
        response.set_recursion_available(true);
        response.set_authoritative_answer(true);

        if (query.get_opcode() != DnsOpcode::QUERY) {
            response.set_response_code(DnsResponseCode::NOT_IMPLEMENTED);
            return;
        }

        bool has_missing_name = false;

        for (const auto &question : query.get_questions()) {
            response.add_question(question);

            if (query_handler_) {
                query_handler_(query, response);
            }

            const auto records = find_records(question.name, question.type);
            if (!records.empty()) {
                for (const auto &record : records) {
                    response.add_answer(record);
                }
                continue;
            }

            if (!has_name(question.name)) {
                has_missing_name = true;
            }
        }

        if (response.get_answers().empty() && has_missing_name &&
            response.get_response_code() == DnsResponseCode::NO_ERROR) {
            response.set_response_code(DnsResponseCode::NAME_ERROR);
        }
    }

    std::vector<DnsResourceRecord> DnsServer::find_records(const std::string & name, DnsType type) const
    {
        const auto key = normalize_name(name);

        std::vector<DnsResourceRecord> matches;
        auto it = dns_records_.find(key);
        if (it == dns_records_.end()) {
            return matches;
        }

        for (const auto &record : it->second) {
            if (type == DnsType::ANY || record.type == type) {
                matches.push_back(record);
            }
        }

        return matches;
    }

    bool DnsServer::has_name(const std::string & name) const
    {
        return dns_records_.find(normalize_name(name)) != dns_records_.end();
    }

    std::string DnsServer::normalize_name(std::string name)
    {
        while (!name.empty() && name.back() == '.') {
            name.pop_back();
        }

        for (auto &ch : name) {
            if (ch >= 'A' && ch <= 'Z') {
                ch = static_cast<char>(ch - 'A' + 'a');
            }
        }

        return name;
    }

    bool DnsServer::serve(int port)
    {
        owned_runtime_ = std::make_unique<NetworkRuntime>();
        return serve(port, *owned_runtime_);
    }

    bool DnsServer::serve(int port, NetworkRuntime & runtime)
    {
        port_ = port;
        if (!session_.bind(port, runtime)) {
            return false;
        }

        running_ = true;

        if (owned_runtime_) {
            runtime.run();
            running_ = false;
            session_.close();
        }

        return true;
    }

    void DnsServer::stop()
    {
        running_ = false;
        if (owned_runtime_) {
            owned_runtime_->stop();
        }
    }

    void DnsServer::set_query_handler(DnsQueryHandler handler)
    {
        query_handler_ = handler;
    }

    void DnsServer::add_record(const std::string & name, const std::string & ip, DnsType type)
    {
        DnsResourceRecord record;
        record.name = normalize_name(name);
        record.type = type;
        record.class_ = DnsClass::IN;
        record.ttl = 3600;
        record.set_rdata_from_string(ip);
        if (record.rdata.empty()) {
            return;
        }

        auto &records = dns_records_[record.name];
        auto it = std::find_if(records.begin(), records.end(), [&record](const DnsResourceRecord &existing) {
            return existing.type == record.type && existing.rdata == record.rdata;
        });
        if (it != records.end()) {
            *it = record;
            return;
        }

        records.push_back(record);
    }

    bool DnsServer::has_record(const std::string & name, DnsType type, const std::string & value) const
    {
        DnsResourceRecord record;
        record.name = normalize_name(name);
        record.type = type;
        record.class_ = DnsClass::IN;
        record.set_rdata_from_string(value);
        if (record.rdata.empty()) {
            return false;
        }

        const auto key = normalize_name(name);
        auto it = dns_records_.find(key);
        if (it == dns_records_.end()) {
            return false;
        }

        return std::any_of(it->second.begin(), it->second.end(), [&record](const DnsResourceRecord &existing) {
            return existing.type == record.type && existing.rdata == record.rdata;
        });
    }
}
