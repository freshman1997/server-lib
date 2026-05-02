#include "buffer/buffer_chain.h"
#include "buffer/byte_buffer.h"
#include "eventbus/event_bus.h"

#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace
{
    using Clock = std::chrono::steady_clock;

    struct BenchResult
    {
        std::string name;
        std::uint64_t operations = 0;
        std::uint64_t bytes = 0;
        std::chrono::nanoseconds elapsed{};
    };

    template <typename Fn>
    BenchResult run_bench(std::string name, std::uint64_t operations, std::uint64_t bytes, Fn fn)
    {
        const auto start = Clock::now();
        fn();
        const auto end = Clock::now();
        return BenchResult{std::move(name), operations, bytes,
                           std::chrono::duration_cast<std::chrono::nanoseconds>(end - start)};
    }

    void print_result(const BenchResult &result)
    {
        const double seconds = static_cast<double>(result.elapsed.count()) / 1'000'000'000.0;
        const double ops_per_second = seconds > 0.0 ? static_cast<double>(result.operations) / seconds : 0.0;
        const double mib_per_second = seconds > 0.0 && result.bytes > 0
                                          ? (static_cast<double>(result.bytes) / (1024.0 * 1024.0)) / seconds
                                          : 0.0;

        std::cout << std::left << std::setw(30) << result.name
                  << " ops=" << std::setw(12) << result.operations
                  << " elapsed_ms=" << std::setw(10)
                  << (static_cast<double>(result.elapsed.count()) / 1'000'000.0)
                  << " ops_per_s=" << std::setw(14) << ops_per_second;
        if (result.bytes > 0) {
            std::cout << " MiB_per_s=" << mib_per_second;
        }
        std::cout << '\n';
    }

    BenchResult bench_byte_buffer_append_copy()
    {
        constexpr std::uint64_t iterations = 200'000;
        constexpr std::size_t payload_size = 1024;
        const std::string payload(payload_size, 'x');
        std::uint64_t checksum = 0;

        auto result = run_bench("byte_buffer_append_copy",
                                iterations,
                                iterations * payload_size,
                                [&]() {
                                    for (std::uint64_t i = 0; i < iterations; ++i) {
                                        yuan::buffer::ByteBuffer buffer(payload_size);
                                        buffer.append(std::string_view(payload));
                                        auto copy = buffer.copy_readable();
                                        checksum += copy.readable_bytes();
                                    }
                                });

        if (checksum != iterations * payload_size) {
            std::cerr << "byte_buffer_append_copy checksum failed\n";
            std::exit(1);
        }
        return result;
    }

    BenchResult bench_buffer_chain_push_pop()
    {
        constexpr std::uint64_t iterations = 100'000;
        constexpr std::size_t chunks_per_iteration = 8;
        constexpr std::size_t payload_size = 512;
        const std::string payload(payload_size, 'y');
        std::uint64_t checksum = 0;

        auto result = run_bench("buffer_chain_push_pop",
                                iterations * chunks_per_iteration,
                                iterations * chunks_per_iteration * payload_size,
                                [&]() {
                                    for (std::uint64_t i = 0; i < iterations; ++i) {
                                        yuan::buffer::BufferChain chain;
                                        for (std::size_t chunk = 0; chunk < chunks_per_iteration; ++chunk) {
                                            auto *buffer = chain.emplace_back(payload_size);
                                            buffer->append(std::string_view(payload));
                                        }
                                        while (!chain.empty()) {
                                            auto front = chain.pop_front();
                                            checksum += front ? front->readable_bytes() : 0;
                                        }
                                    }
                                });

        if (checksum != iterations * chunks_per_iteration * payload_size) {
            std::cerr << "buffer_chain_push_pop checksum failed\n";
            std::exit(1);
        }
        return result;
    }

    BenchResult bench_event_bus_publish()
    {
        constexpr std::uint64_t iterations = 300'000;
        constexpr int subscribers = 4;
        yuan::eventbus::EventBus bus;
        std::uint64_t checksum = 0;

        for (int i = 0; i < subscribers; ++i) {
            bus.subscribe("core.benchmark", [&checksum](const yuan::eventbus::Event &) {
                ++checksum;
            });
        }

        auto result = run_bench("event_bus_publish",
                                iterations,
                                0,
                                [&]() {
                                    for (std::uint64_t i = 0; i < iterations; ++i) {
                                        bus.publish("core.benchmark");
                                    }
                                });

        if (checksum != iterations * subscribers) {
            std::cerr << "event_bus_publish checksum failed\n";
            std::exit(1);
        }
        return result;
    }
}

int main()
{
    std::cout << "core runtime benchmark\n";
    std::cout << "build=manual chrono=steady_clock\n";

    std::vector<BenchResult> results;
    results.push_back(bench_byte_buffer_append_copy());
    results.push_back(bench_buffer_chain_push_pop());
    results.push_back(bench_event_bus_publish());

    for (const auto &result : results) {
        print_result(result);
    }

    return 0;
}
