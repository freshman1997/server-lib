#ifndef __YUAN_NET_ASYNC_ASYNC_REQUEST_CLIENT_H__
#define __YUAN_NET_ASYNC_ASYNC_REQUEST_CLIENT_H__

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "coroutine/io_result.h"
#include "coroutine/runtime.h"
#include "coroutine/task.h"
#include "net/async/async_client_session.h"
#include "net/runtime/network_runtime.h"

namespace yuan::net
{

    class AsyncRequestClient
    {
    public:
        using RequestTransformer = std::function< ::yuan::buffer::ByteBuffer(const ::yuan::buffer::ByteBuffer &)>;
        using ResponseTransformer = std::function< ::yuan::buffer::ByteBuffer(::yuan::buffer::ByteBuffer)>;

        AsyncRequestClient() = default;

        explicit AsyncRequestClient(coroutine::RuntimeView runtime)
            : runtime_(runtime)
        {
        }

        ~AsyncRequestClient() = default;

        AsyncRequestClient(const AsyncRequestClient &) = delete;
        AsyncRequestClient &operator=(const AsyncRequestClient &) = delete;

        void set_runtime(coroutine::RuntimeView runtime) noexcept
        {
            runtime_ = runtime;
        }

        void set_request_transformer(RequestTransformer transformer)
        {
            request_transformer_ = std::move(transformer);
        }

        void set_response_transformer(ResponseTransformer transformer)
        {
            response_transformer_ = std::move(transformer);
        }

        coroutine::Task<coroutine::ReadResult> request_async(
            const std::string &host,
            uint16_t port,
            const ::yuan::buffer::ByteBuffer &request_buf,
            uint32_t connect_timeout_ms = 0,
            uint32_t read_timeout_ms = 0)
        {
            AsyncClientSession session;

            bool connected = co_await session.connect_async(runtime_, host, port, connect_timeout_ms);
            if (!connected) {
                co_return coroutine::ReadResult::with_status(coroutine::IoStatus::connection_error);
            }

            const auto &write_buf = request_transformer_
                                        ? request_transformer_(request_buf)
                                        : request_buf;

            auto write_result = co_await session.write_async(write_buf);
            if (write_result.status != coroutine::IoStatus::success) {
                co_await session.close_async();
                co_return coroutine::ReadResult::with_status(write_result.status);
            }

            auto read_result = co_await session.read_awaiter(read_timeout_ms);

            if (response_transformer_ && read_result.status == coroutine::IoStatus::success) {
                read_result.data = response_transformer_(std::move(read_result.data));
            }

            co_await session.close_async();
            co_return read_result;
        }

        coroutine::Task<coroutine::ReadResult> request_async(
            const ::yuan::buffer::ByteBuffer &request_buf,
            uint32_t read_timeout_ms = 0)
        {
            if (!session_.is_connected()) {
                co_return coroutine::ReadResult::with_status(coroutine::IoStatus::invalid_state);
            }

            const auto &write_buf = request_transformer_
                                        ? request_transformer_(request_buf)
                                        : request_buf;

            auto write_result = co_await session_.write_async(write_buf);
            if (write_result.status != coroutine::IoStatus::success) {
                co_return coroutine::ReadResult::with_status(write_result.status);
            }

            auto read_result = co_await session_.read_awaiter(read_timeout_ms);

            if (response_transformer_ && read_result.status == coroutine::IoStatus::success) {
                read_result.data = response_transformer_(std::move(read_result.data));
            }

            co_return read_result;
        }

        coroutine::Task<bool> connect_async(const std::string &host, uint16_t port, uint32_t timeout_ms = 0)
        {
            bool connected = co_await session_.connect_async(runtime_, host, port, timeout_ms);
            co_return connected;
        }

        void disconnect()
        {
            session_.close();
        }

        bool is_connected() const noexcept
        {
            return session_.is_connected();
        }

        AsyncClientSession &session() noexcept
        {
            return session_;
        }

    private:
        coroutine::RuntimeView runtime_{};
        AsyncClientSession session_;
        RequestTransformer request_transformer_;
        ResponseTransformer response_transformer_;
    };

} // namespace yuan::net

#endif
