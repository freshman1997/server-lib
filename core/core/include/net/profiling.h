#ifndef YUAN_NET_PROFILING_H
#define YUAN_NET_PROFILING_H

#include <atomic>
#include <cstddef>
#include <cstdint>

#if defined(YUAN_ENABLE_TRACY)
#include <tracy/Tracy.hpp>
#define YUAN_NET_PROFILE_ZONE(name) ZoneScopedN(name)
#define YUAN_NET_PROFILE_TCP_SEND(...) ::yuan::net::profiling::record_tcp_send(__VA_ARGS__)
#define YUAN_NET_PROFILE_TCP_FLUSH(...) ::yuan::net::profiling::record_tcp_flush(__VA_ARGS__)

namespace yuan::net::profiling
{
    struct TcpMetrics
    {
        std::uint64_t direct_send_calls = 0;
        std::uint64_t flush_send_calls = 0;
        std::uint64_t direct_requested_bytes = 0;
        std::uint64_t flush_requested_bytes = 0;
        std::uint64_t direct_sent_bytes = 0;
        std::uint64_t flush_sent_bytes = 0;
        std::uint64_t direct_partial_calls = 0;
        std::uint64_t flush_partial_calls = 0;
        std::uint64_t direct_transient_errors = 0;
        std::uint64_t flush_transient_errors = 0;
        std::uint64_t flush_calls = 0;
        std::uint64_t flush_queue_chunks = 0;
        std::uint64_t flush_queue_bytes = 0;
        std::uint64_t flush_queue_max_chunks = 0;
        std::uint64_t flush_queue_max_bytes = 0;
    };

    inline std::atomic_uint64_t tcp_direct_send_calls = 0;
    inline std::atomic_uint64_t tcp_flush_send_calls = 0;
    inline std::atomic_uint64_t tcp_direct_requested_bytes = 0;
    inline std::atomic_uint64_t tcp_flush_requested_bytes = 0;
    inline std::atomic_uint64_t tcp_direct_sent_bytes = 0;
    inline std::atomic_uint64_t tcp_flush_sent_bytes = 0;
    inline std::atomic_uint64_t tcp_direct_partial_calls = 0;
    inline std::atomic_uint64_t tcp_flush_partial_calls = 0;
    inline std::atomic_uint64_t tcp_direct_transient_errors = 0;
    inline std::atomic_uint64_t tcp_flush_transient_errors = 0;
    inline std::atomic_uint64_t tcp_flush_calls = 0;
    inline std::atomic_uint64_t tcp_flush_queue_chunks = 0;
    inline std::atomic_uint64_t tcp_flush_queue_bytes = 0;
    inline std::atomic_uint64_t tcp_flush_queue_max_chunks = 0;
    inline std::atomic_uint64_t tcp_flush_queue_max_bytes = 0;
    inline std::atomic_uint64_t tcp_plot_sample = 0;

    inline TcpMetrics tcp_metrics()
    {
        return {
            tcp_direct_send_calls.load(std::memory_order_relaxed),
            tcp_flush_send_calls.load(std::memory_order_relaxed),
            tcp_direct_requested_bytes.load(std::memory_order_relaxed),
            tcp_flush_requested_bytes.load(std::memory_order_relaxed),
            tcp_direct_sent_bytes.load(std::memory_order_relaxed),
            tcp_flush_sent_bytes.load(std::memory_order_relaxed),
            tcp_direct_partial_calls.load(std::memory_order_relaxed),
            tcp_flush_partial_calls.load(std::memory_order_relaxed),
            tcp_direct_transient_errors.load(std::memory_order_relaxed),
            tcp_flush_transient_errors.load(std::memory_order_relaxed),
            tcp_flush_calls.load(std::memory_order_relaxed),
            tcp_flush_queue_chunks.load(std::memory_order_relaxed),
            tcp_flush_queue_bytes.load(std::memory_order_relaxed),
            tcp_flush_queue_max_chunks.load(std::memory_order_relaxed),
            tcp_flush_queue_max_bytes.load(std::memory_order_relaxed)};
    }

    inline void reset_tcp_metrics()
    {
        tcp_direct_send_calls.store(0, std::memory_order_relaxed);
        tcp_flush_send_calls.store(0, std::memory_order_relaxed);
        tcp_direct_requested_bytes.store(0, std::memory_order_relaxed);
        tcp_flush_requested_bytes.store(0, std::memory_order_relaxed);
        tcp_direct_sent_bytes.store(0, std::memory_order_relaxed);
        tcp_flush_sent_bytes.store(0, std::memory_order_relaxed);
        tcp_direct_partial_calls.store(0, std::memory_order_relaxed);
        tcp_flush_partial_calls.store(0, std::memory_order_relaxed);
        tcp_direct_transient_errors.store(0, std::memory_order_relaxed);
        tcp_flush_transient_errors.store(0, std::memory_order_relaxed);
        tcp_flush_calls.store(0, std::memory_order_relaxed);
        tcp_flush_queue_chunks.store(0, std::memory_order_relaxed);
        tcp_flush_queue_bytes.store(0, std::memory_order_relaxed);
        tcp_flush_queue_max_chunks.store(0, std::memory_order_relaxed);
        tcp_flush_queue_max_bytes.store(0, std::memory_order_relaxed);
        tcp_plot_sample.store(0, std::memory_order_relaxed);
    }

    inline void plot_tcp_metrics()
    {
        TracyPlot("tcp.direct_send_calls", static_cast<std::int64_t>(tcp_direct_send_calls.load()));
        TracyPlot("tcp.flush_send_calls", static_cast<std::int64_t>(tcp_flush_send_calls.load()));
        TracyPlot("tcp.direct_requested_bytes", static_cast<std::int64_t>(tcp_direct_requested_bytes.load()));
        TracyPlot("tcp.flush_requested_bytes", static_cast<std::int64_t>(tcp_flush_requested_bytes.load()));
        TracyPlot("tcp.direct_sent_bytes", static_cast<std::int64_t>(tcp_direct_sent_bytes.load()));
        TracyPlot("tcp.flush_sent_bytes", static_cast<std::int64_t>(tcp_flush_sent_bytes.load()));
        TracyPlot("tcp.direct_partial_calls", static_cast<std::int64_t>(tcp_direct_partial_calls.load()));
        TracyPlot("tcp.flush_partial_calls", static_cast<std::int64_t>(tcp_flush_partial_calls.load()));
        TracyPlot("tcp.direct_transient_errors", static_cast<std::int64_t>(tcp_direct_transient_errors.load()));
        TracyPlot("tcp.flush_transient_errors", static_cast<std::int64_t>(tcp_flush_transient_errors.load()));
        TracyPlot("tcp.flush_calls", static_cast<std::int64_t>(tcp_flush_calls.load()));
        TracyPlot("tcp.flush_queue_chunks", static_cast<std::int64_t>(tcp_flush_queue_chunks.load()));
        TracyPlot("tcp.flush_queue_bytes", static_cast<std::int64_t>(tcp_flush_queue_bytes.load()));
        TracyPlot("tcp.flush_queue_max_chunks", static_cast<std::int64_t>(tcp_flush_queue_max_chunks.load()));
        TracyPlot("tcp.flush_queue_max_bytes", static_cast<std::int64_t>(tcp_flush_queue_max_bytes.load()));
    }

    inline void record_tcp_flush(std::size_t chunks, std::size_t bytes)
    {
        ++tcp_flush_calls;
        tcp_flush_queue_chunks += chunks;
        tcp_flush_queue_bytes += bytes;
        auto max_chunks = tcp_flush_queue_max_chunks.load();
        while (chunks > max_chunks && !tcp_flush_queue_max_chunks.compare_exchange_weak(max_chunks, chunks)) {}
        auto max_bytes = tcp_flush_queue_max_bytes.load();
        while (bytes > max_bytes && !tcp_flush_queue_max_bytes.compare_exchange_weak(max_bytes, bytes)) {}
        if ((tcp_plot_sample.fetch_add(1, std::memory_order_relaxed) & 1023u) == 0) plot_tcp_metrics();
    }

    inline void record_tcp_send(bool direct, std::size_t requested, int result, bool transient)
    {
        auto &calls = direct ? tcp_direct_send_calls : tcp_flush_send_calls;
        auto &requested_bytes = direct ? tcp_direct_requested_bytes : tcp_flush_requested_bytes;
        auto &sent_bytes = direct ? tcp_direct_sent_bytes : tcp_flush_sent_bytes;
        auto &partial_calls = direct ? tcp_direct_partial_calls : tcp_flush_partial_calls;
        auto &transient_errors = direct ? tcp_direct_transient_errors : tcp_flush_transient_errors;
        calls.fetch_add(1, std::memory_order_relaxed);
        requested_bytes.fetch_add(requested, std::memory_order_relaxed);
        if (result > 0) {
            sent_bytes.fetch_add(static_cast<std::size_t>(result), std::memory_order_relaxed);
            if (static_cast<std::size_t>(result) < requested) partial_calls.fetch_add(1, std::memory_order_relaxed);
        } else if (transient) {
            transient_errors.fetch_add(1, std::memory_order_relaxed);
        }
        if ((tcp_plot_sample.fetch_add(1, std::memory_order_relaxed) & 1023u) == 0) plot_tcp_metrics();
    }
}
#else
#define YUAN_NET_PROFILE_ZONE(name) ((void)0)
#define YUAN_NET_PROFILE_TCP_SEND(...) ((void)0)
#define YUAN_NET_PROFILE_TCP_FLUSH(...) ((void)0)
#endif

#endif
