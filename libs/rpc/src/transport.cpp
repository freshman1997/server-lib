#include "yuan/rpc/transport.h"

#include <utility>

namespace yuan::rpc
{
    void LoopbackTransport::connect(LoopbackTransport &peer)
    {
        peer_ = &peer;
    }

    bool LoopbackTransport::send_frame(const Bytes &frame)
    {
        auto *peer = peer_;
        if (!open_ || !peer || !peer->open_) {
            return false;
        }
        if (peer->receive_callback_) {
            peer->receive_callback_(frame.data(), frame.size());
        }
        return true;
    }

    bool LoopbackTransport::is_open() const
    {
        return open_;
    }

    void LoopbackTransport::close()
    {
        if (!open_) {
            return;
        }
        open_ = false;
        if (close_callback_) {
            close_callback_();
        }
    }

    void LoopbackTransport::set_receive_callback(ReceiveCallback callback)
    {
        receive_callback_ = std::move(callback);
    }

    void LoopbackTransport::set_close_callback(CloseCallback callback)
    {
        close_callback_ = std::move(callback);
    }

    bool BlackholeTransport::send_frame(const Bytes &frame)
    {
        (void)frame;
        return open_;
    }

    bool BlackholeTransport::is_open() const
    {
        return open_;
    }

    void BlackholeTransport::close()
    {
        if (!open_) {
            return;
        }
        open_ = false;
        if (close_callback_) {
            close_callback_();
        }
    }

    void BlackholeTransport::set_receive_callback(ReceiveCallback callback)
    {
        receive_callback_ = std::move(callback);
    }

    void BlackholeTransport::set_close_callback(CloseCallback callback)
    {
        close_callback_ = std::move(callback);
    }
}
