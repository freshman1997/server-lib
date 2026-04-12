#include "application.h"
#include "bootstrap.h"
#include "match_service.h"

#include <atomic>
#include <csignal>
#include <cstdint>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>

namespace
{

std::atomic<bool> g_running{true};

void signal_handler(int)
{
    g_running.store(false);
}

void print_usage()
{
    std::cout << "Match Server Usage:\n"
                 "  create <player_id> <score>\n"
                 "  join <node_id> <player_id> <score>\n"
                 "  start <node_id> <mode>\n"
                 "  cancel <node_id>\n"
                 "  stats\n"
                 "  help\n"
                 "  quit\n";
}

void process_command(match::MatchServer& server, const std::string& line)
{
    std::istringstream input(line);
    std::string command;
    input >> command;

    if (command == "stats") {
        std::cout << server.get_statistics() << '\n';
        return;
    }

    if (command == "help") {
        print_usage();
        return;
    }

    if (command == "quit" || command == "exit") {
        g_running.store(false);
        return;
    }

    if (command == "create") {
        std::uint64_t player_id = 0;
        std::uint32_t score = 0;
        if ((input >> player_id >> score) && server.create_node(player_id, score)) {
            std::cout << "created node for player " << player_id << '\n';
        } else {
            std::cout << "usage: create <player_id> <score>\n";
        }
        return;
    }

    if (command == "join") {
        std::uint64_t node_id = 0;
        std::uint64_t player_id = 0;
        std::uint32_t score = 0;
        if ((input >> node_id >> player_id >> score) &&
            server.add_player_to_node(node_id, player_id, score)) {
            std::cout << "player " << player_id << " joined node " << node_id << '\n';
        } else {
            std::cout << "usage: join <node_id> <player_id> <score>\n";
        }
        return;
    }

    if (command == "start") {
        std::uint64_t node_id = 0;
        int mode = 0;
        if ((input >> node_id >> mode) &&
            server.start_match(node_id, static_cast<match::GameMode>(mode))) {
            std::cout << "node " << node_id << " started matching\n";
        } else {
            std::cout << "usage: start <node_id> <mode>\n";
        }
        return;
    }

    if (command == "cancel") {
        std::uint64_t node_id = 0;
        if ((input >> node_id) && server.cancel_match(node_id)) {
            std::cout << "node " << node_id << " canceled\n";
        } else {
            std::cout << "usage: cancel <node_id>\n";
        }
        return;
    }

    std::cout << "unknown command, type 'help'\n";
}

} // namespace

int main(int argc, char* argv[])
{
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    std::string config_path = "config/match_config.json";
    if (argc > 1) {
        config_path = argv[1];
    }

    yuan::app::RuntimeContext context;
    context.app_name = "match-server";

    yuan::app::Application application(context);
    auto service = std::make_shared<match::MatchService>(config_path);
    if (!application.add_typed_service<match::MatchService>("match", service, "server.match", 1)) {
        std::cerr << "failed to register match service\n";
        return 1;
    }

    yuan::app::Bootstrap bootstrap(application);
    if (!bootstrap.run()) {
        std::cerr << "failed to start match service\n";
        return 1;
    }

    const auto snapshot = bootstrap.supervisor_snapshot();
    std::cout << "process role: " << yuan::app::to_string(bootstrap.process_role())
              << ", supervisor_state=" << yuan::app::to_string(snapshot.state)
              << ", supervisor_reason=" << yuan::app::to_string(snapshot.reason)
              << ", worker_index=" << application.context().worker_index
              << ", is_worker_process=" << (application.context().is_worker_process ? "true" : "false")
              << ", owns_runtime=" << bootstrap.owns_runtime()
              << ", running_workers=" << snapshot.running_workers
              << ", recovering_workers=" << snapshot.recovering_workers
              << ", suppressed_workers=" << snapshot.suppressed_workers
              << ", failed_workers=" << snapshot.failed_workers
              << ", total_restarts=" << snapshot.total_restarts
              << ", shutdown_started=" << (snapshot.shutdown_started ? "true" : "false")
              << '\n';

    if (bootstrap.process_role() == yuan::app::ProcessRole::standalone) {
        print_usage();

        std::string line;
        while (g_running.load() && std::getline(std::cin, line)) {
            process_command(service->server(), line);
        }
    } else {
        while (g_running.load()) {
            bootstrap.poll_workers();
            if (bootstrap.process_role() == yuan::app::ProcessRole::supervisor &&
                (bootstrap.has_failed_workers() ||
                 (!bootstrap.has_running_workers() && !bootstrap.has_recovering_workers()))) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
    }

    bootstrap.shutdown();
    return 0;
}
