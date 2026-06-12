#include "base/utils/simple_search.h"

#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

namespace
{
    using Clock = std::chrono::steady_clock;
    using Search = yuan::base::SimpleSearch<std::uint64_t>;

    struct Result
    {
        std::size_t size = 0;
        double insert_ms = 0.0;
        double prefix_us = 0.0;
        double contains_us = 0.0;
        double fuzzy_us = 0.0;
        double exhaustive_contains_ms = 0.0;
    };

    template <typename Fn>
    double elapsed_ms(Fn fn)
    {
        const auto start = Clock::now();
        fn();
        const auto end = Clock::now();
        return static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count()) / 1'000'000.0;
    }

    std::string make_text(std::size_t i)
    {
        return "guild_" + std::to_string(i) + "_dragon_zone_" + std::to_string(i % 97);
    }

    Result run_case(std::size_t size)
    {
        Search search;
        Result result;
        result.size = size;

        result.insert_ms = elapsed_ms([&]() {
            for (std::size_t i = 0; i < size; ++i) {
                search.insert(static_cast<std::uint64_t>(i), make_text(i));
            }
        });

        constexpr int iterations = 1000;
        std::size_t checksum = 0;

        result.prefix_us = elapsed_ms([&]() {
            for (int i = 0; i < iterations; ++i) {
                checksum += search.prefix_search("guild_99", 20).size();
            }
        }) * 1000.0 / iterations;

        result.contains_us = elapsed_ms([&]() {
            for (int i = 0; i < iterations; ++i) {
                checksum += search.contains_search("dragon_zone", 20).size();
            }
        }) * 1000.0 / iterations;

        result.fuzzy_us = elapsed_ms([&]() {
            for (int i = 0; i < iterations; ++i) {
                checksum += search.search("guild", 20).size();
            }
        }) * 1000.0 / iterations;

        result.exhaustive_contains_ms = elapsed_ms([&]() {
            checksum += search.contains_search("dragon_zone", 0).size();
        });

        if (checksum == 0) {
            std::cerr << "simple_search_benchmark checksum failed\n";
            std::exit(1);
        }

        return result;
    }

    void print_header()
    {
        std::cout << std::left
                  << std::setw(10) << "N"
                  << std::setw(14) << "insert_ms"
                  << std::setw(14) << "prefix_us"
                  << std::setw(14) << "contains_us"
                  << std::setw(14) << "fuzzy_us"
                  << std::setw(24) << "contains_all_ms"
                  << '\n';
    }

    void print_result(const Result &result)
    {
        std::cout << std::left << std::fixed << std::setprecision(3)
                  << std::setw(10) << result.size
                  << std::setw(14) << result.insert_ms
                  << std::setw(14) << result.prefix_us
                  << std::setw(14) << result.contains_us
                  << std::setw(14) << result.fuzzy_us
                  << std::setw(24) << result.exhaustive_contains_ms
                  << '\n';
    }
}

int main()
{
    const std::vector<std::size_t> sizes{1000, 10000, 50000};
    print_header();
    for (const auto size : sizes) {
        print_result(run_case(size));
    }
    return 0;
}
