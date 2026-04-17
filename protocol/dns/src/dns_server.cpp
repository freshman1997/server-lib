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

        for (const auto &question : query.get_questions()) {
            response.add_question(question);

            if (query_handler_) {
                query_handler_(query, response);
            }

            if (response.get_answers().empty()) {
                DnsResourceRecord record = find_record(question.name, question.type);
                if (record.name.empty()) {
                    response.set_response_code(DnsResponseCode::NAME_ERROR);
                } else {
                    response.add_answer(record);
                }
            }
        }

        if (response.get_answers().empty() && response.get_response_code() == DnsResponseCode::NO_ERROR) {
            response.set_response_code(DnsResponseCode::NAME_ERROR);
        }
    }

    DnsResourceRecord DnsServer::find_record(const std::string & name, DnsType type)
    {
        auto key = name;
        std::transform(key.begin(), key.end(), key.begin(), ::tolower);

        auto it = dns_records_.find(key);
        if (it != dns_records_.end()) {
            if (it->second.type == type || type == DnsType::ANY) {
                return it->second;
            }
        }

        return DnsResourceRecord();
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
        record.name = name;
        record.type = type;
        record.class_ = DnsClass::IN;
        record.ttl = 3600;
        record.set_rdata_from_string(ip);

        auto key = name;
        std::transform(key.begin(), key.end(), key.begin(), ::tolower);
        dns_records_[key] = record;
    }
}
