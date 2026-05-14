#include "rtsp_server.h"
#include "rtsp_server_config.h"

#include <chrono>
#include <csignal>
#include <iostream>
#include <string>
#include <thread>

namespace
{
volatile sig_atomic_t g_running = 1;

void signal_handler(int)
{
    g_running = 0;
}
} // namespace

int main(int argc, char **argv)
{
#ifndef _WIN32
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);
    std::signal(SIGPIPE, SIG_IGN);
#endif

    yuan::release::rtsp::ParseResult parse_result;
    std::string parse_error;
    if (!yuan::release::rtsp::parse_server_options(argc, argv, parse_result, parse_error)) {
        std::cerr << parse_error << '\n';
        yuan::release::rtsp::print_usage(argv[0]);
        return 2;
    }

    if (parse_result.mode == yuan::release::rtsp::ParseMode::print_help) {
        yuan::release::rtsp::print_usage(argv[0]);
        return 0;
    }
    if (parse_result.mode == yuan::release::rtsp::ParseMode::print_version) {
        std::cout << yuan::release::rtsp::version_string() << '\n';
        return 0;
    }

    const auto &config = parse_result.config;

    yuan::net::rtsp::RtspServer server;
    yuan::net::rtsp::RtspObservabilityConfig obs;
    obs.enable_log = config.enable_log;
    obs.enable_audit = config.enable_audit;
    obs.max_audit_events = config.max_audit_events;
    obs.udp_retry_max_retries = config.udp_retry_max_retries;
    obs.udp_retry_base_backoff_ms = config.udp_retry_base_backoff_ms;
    obs.udp_retry_max_backoff_ms = config.udp_retry_max_backoff_ms;
    server.configure_observability(obs);

    if (!server.init(config.port)) {
        std::cerr << "failed to start rtsp server on port " << config.port << '\n';
        return 1;
    }

    std::cout << config.app_name << " listening on 0.0.0.0:" << config.port << '\n';
    std::cout << "audit=" << (config.enable_audit ? "on" : "off")
              << " max_audit_events=" << config.max_audit_events
              << " log=" << (config.enable_log ? "on" : "off") << '\n';
    std::cout << "udp_retry max=" << config.udp_retry_max_retries
              << " base_ms=" << config.udp_retry_base_backoff_ms
              << " max_ms=" << config.udp_retry_max_backoff_ms << '\n';

    std::thread server_thread([&server]() {
        server.serve();
    });

    while (g_running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    server.stop();
    if (server_thread.joinable()) {
        server_thread.join();
    }
    return 0;
}
