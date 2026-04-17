#include "dns_client.h"
#include "dns_packet.h"
#include "buffer/byte_buffer.h"
#include "coroutine/sync_wait.h"
#include "logger.h"

namespace yuan::net::dns
{
    DnsClient::DnsClient() = default;

    DnsClient::~DnsClient()
    {
        disconnect();
    }

    bool DnsClient::connect(const std::string & ip, short port)
    {
        owned_runtime_ = std::make_unique<NetworkRuntime>();
        return client_.connect(ip, port, *owned_runtime_);
    }

    bool DnsClient::connect(const std::string & ip, short port, NetworkRuntime & runtime)
    {
        return client_.connect(ip, port, runtime);
    }

    void DnsClient::disconnect()
    {
        client_.close();
        if (owned_runtime_) {
            owned_runtime_->stop();
            owned_runtime_.reset();
        }
    }

    yuan::buffer::ByteBuffer DnsClient::build_query_packet(const std::string & domain, DnsType type, uint16_t session_id)
    {
        DnsPacket packet;
        packet.set_session_id(session_id);
        packet.set_is_response(false);
        packet.set_recursion_desired(true);

        DnsQuestion question;
        question.name = domain;
        question.type = type;
        question.class_ = DnsClass::IN;
        packet.add_question(question);

        yuan::buffer::ByteBuffer byte_buffer;
        packet.serialize(byte_buffer);
        return byte_buffer;
    }

    yuan::coroutine::Task<DnsPacket> DnsClient::query_async(
        const std::string & domain,
        DnsType type,
        uint32_t timeout_ms)
    {
        if (!client_.is_connected()) {
            co_return DnsPacket();
        }

        uint16_t session_id = next_session_id_++;
        auto send_result = client_.send(build_query_packet(domain, type, session_id));
        if (send_result.status != coroutine::IoStatus::success) {
            co_return DnsPacket();
        }

        auto read_result = co_await client_.receive_async(timeout_ms);
        if (read_result.status != coroutine::IoStatus::success || read_result.data.readable_bytes() == 0) {
            co_return DnsPacket();
        }

        DnsPacket response;
        if (!response.deserialize(read_result.data)) {
            co_return DnsPacket();
        }

        last_response_ = response;
        co_return response;
    }

    bool DnsClient::query(const std::string & domain, DnsType type, uint32_t timeout_ms)
    {
        return query(domain, type, nullptr, timeout_ms);
    }

    bool DnsClient::query(const std::string & domain, DnsType type, DnsResponseHandler handler, uint32_t timeout_ms)
    {
        if (!client_.is_connected()) {
            LOG_WARN("DNS client: not connected");
            return false;
        }

        auto rv = client_.runtime_view();
        if (!rv.event_loop()) {
            LOG_WARN("DNS client: no runtime");
            return false;
        }

        auto response = yuan::coroutine::sync_wait(
            rv,
            query_async(domain, type, timeout_ms));

        bool success = response.is_response();

        if (handler) {
            handler(response);
        }

        return success;
    }

    const DnsPacket &DnsClient::get_last_response() const
    {
        return last_response_;
    }
}
