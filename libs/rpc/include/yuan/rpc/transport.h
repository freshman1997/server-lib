#ifndef YUAN_RPC_TRANSPORT_H
#define YUAN_RPC_TRANSPORT_H

#include "types.h"

#include <functional>
#include <memory>
#include <mutex>

namespace yuan::rpc
{
    struct TransportStats
    {
        std::uint64_t frames_sent = 0;
        std::uint64_t frames_received = 0;
        std::uint64_t bytes_sent = 0;
        std::uint64_t bytes_received = 0;
        std::uint64_t decode_errors = 0;
        std::uint64_t send_errors = 0;
    };

    class IFrameTransport
    {
    public:
        using ReceiveCallback = std::function<void(const std::uint8_t *, std::size_t)>;
        using CloseCallback = std::function<void()>;

        virtual ~IFrameTransport() = default;

        virtual bool send_frame(const Bytes &frame) = 0;
        virtual bool is_open() const = 0;
        virtual void close() = 0;

        virtual void set_receive_callback(ReceiveCallback callback) = 0;
        virtual void set_close_callback(CloseCallback callback) = 0;
    };

    class LoopbackTransport final : public IFrameTransport
    {
    public:
        void connect(LoopbackTransport &peer);

        bool send_frame(const Bytes &frame) override;

        bool is_open() const override;

        void close() override;

        void set_receive_callback(ReceiveCallback callback) override;

        void set_close_callback(CloseCallback callback) override;

    private:
        LoopbackTransport *peer_ = nullptr;
        bool open_ = true;
        ReceiveCallback receive_callback_;
        CloseCallback close_callback_;
    };

    class BlackholeTransport final : public IFrameTransport
    {
    public:
        bool send_frame(const Bytes &frame) override;

        bool is_open() const override;

        void close() override;

        void set_receive_callback(ReceiveCallback callback) override;

        void set_close_callback(CloseCallback callback) override;

    private:
        bool open_ = true;
        ReceiveCallback receive_callback_;
        CloseCallback close_callback_;
    };
}

#endif
