#include "repl.h"
#include "formatter.h"
#include "input.h"

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#ifndef _WIN32
#include <sys/select.h>
#include <unistd.h>
#else
#include <conio.h>
#endif

#include "option.h"
#include "redis_client.h"
#include "cmd/pipeline_cmd.h"
#include "value/array_value.h"
#include "value/error_value.h"
#include "value/null_value.h"
#include "value/status_value.h"
#include "value/string_value.h"
#include "value/int_value.h"
#include "value/map_value.h"

namespace yredis
{
    std::string to_lower(std::string s)
    {
        std::transform(s.begin(), s.end(), s.begin(), ::tolower);
        return s;
    }

    static int safe_stoi(const std::string &s, int default_val = 0)
    {
        try
        {
            return std::stoi(s);
        }
        catch (const std::exception &)
        {
            std::cout << ansi_red() << "Invalid number: " << s << ansi_reset() << "\n";
            return default_val;
        }
    }

    static volatile sig_atomic_t g_interrupted = 0;

    static void sigint_handler(int)
    {
        g_interrupted = 1;
    }

    static bool stdin_has_data()
    {
#ifdef _WIN32
        return _kbhit() != 0;
#else
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(STDIN_FILENO, &rfds);
        struct timeval tv = {0, 0};
        return select(STDIN_FILENO + 1, &rfds, nullptr, nullptr, &tv) > 0;
#endif
    }

    std::vector<std::string> tokenize(const std::string &line)
    {
        std::vector<std::string> tokens;
        std::string current;
        bool in_quote = false;
        char quote_char = 0;
        bool escaping = false;

        for (char c : line)
        {
            if (escaping)
            {
                current += c;
                escaping = false;
                continue;
            }

            if (c == '\\' && in_quote)
            {
                escaping = true;
                continue;
            }

            if (in_quote)
            {
                if (c == quote_char)
                {
                    in_quote = false;
                    if (!current.empty())
                    {
                        tokens.push_back(current);
                        current.clear();
                    }
                }
                else
                {
                    current += c;
                }
            }
            else
            {
                if (c == '"' || c == '\'')
                {
                    in_quote = true;
                    quote_char = c;
                }
                else if (c == ' ' || c == '\t')
                {
                    if (!current.empty())
                    {
                        tokens.push_back(current);
                        current.clear();
                    }
                }
                else
                {
                    current += c;
                }
            }
        }

        if (in_quote)
        {
            if (!current.empty())
            {
                tokens.push_back(current);
            }
        }
        else if (!current.empty())
        {
            tokens.push_back(current);
        }

        return tokens;
    }

    static std::string normalize_meta(const std::string &first_token)
    {
        auto lower = to_lower(first_token);
        if (lower.size() > 1 && lower[0] == '\\') return lower.substr(1);
        return lower;
    }

    bool is_meta_command(const std::string &first_token)
    {
        auto lower = to_lower(first_token);
        if (lower.size() > 1 && lower[0] == '\\')
        {
            auto cmd = lower.substr(1);
            return cmd == "help" || cmd == "h" ||
                   cmd == "quit" || cmd == "q" ||
                   cmd == "connect" || cmd == "c" ||
                   cmd == "disconnect" || cmd == "d" ||
                   cmd == "status" || cmd == "s" ||
                   cmd == "stats" ||
                   cmd == "format" || cmd == "f" ||
                   cmd == "pipeline" || cmd == "p" ||
                   cmd == "exec" || cmd == "e" ||
                   cmd == "abort" || cmd == "a" ||
                   cmd == "ping" ||
                   cmd == "timeout" ||
                   cmd == "reconnect" ||
                   cmd == "db" ||
                   cmd == "history" ||
                   cmd == "scan-keys" ||
                   cmd == "key-info" ||
                   cmd == "export";
        }

        return lower == "help" || lower == "h" ||
               lower == "quit" || lower == "q" || lower == "exit" ||
               lower == "connect" || lower == "c" ||
               lower == "disconnect" || lower == "d" ||
               lower == "status" || lower == "s" ||
               lower == "stats" ||
               lower == "format" || lower == "f" ||
               lower == "pipeline" || lower == "p" ||
               lower == "timeout" ||
               lower == "reconnect" ||
               lower == "db" ||
               lower == "history" ||
               lower == "scan-keys" ||
               lower == "key-info" ||
               lower == "export";
    }

    void print_banner()
    {
        std::cout << ansi_bold() << ansi_cyan()
                  << "yredis - Redis CLI Tool (yuan::redis)" << ansi_reset() << "\n"
                  << ansi_gray() << "Type help for built-in commands, or any Redis command to execute." << ansi_reset() << "\n";
#ifdef HAVE_READLINE
        if (is_interactive())
        {
            std::cout << ansi_gray() << "Tab-completion and history enabled. Use history to view." << ansi_reset() << "\n";
        }
#endif
        std::cout << "\n";
    }

    void print_help()
    {
        std::cout << ansi_bold() << "Built-in Commands (with or without \\ prefix):" << ansi_reset() << "\n"
                  << ansi_cyan() << "  help, \\h" << ansi_reset() << "              Show this help\n"
                  << ansi_cyan() << "  quit, \\q, exit" << ansi_reset() << "      Quit yredis\n"
                  << ansi_cyan() << "  connect, \\c" << ansi_reset() << "          Connect to Redis (default: localhost:6379)\n"
                  << ansi_cyan() << "  disconnect, \\d" << ansi_reset() << "    Disconnect from Redis\n"
                  << ansi_cyan() << "  status, \\s" << ansi_reset() << "         Show connection status\n"
                  << ansi_cyan() << "  stats" << ansi_reset() << "                  Show client stats\n"
                  << ansi_cyan() << "  format, \\f" << ansi_reset() << "         Set output format (pretty|raw)\n"
                  << ansi_cyan() << "  pipeline, \\p" << ansi_reset() << "       Enter pipeline mode\n"
                  << ansi_cyan() << "  \\ping" << ansi_reset() << "                 Ping Redis server (meta; plain 'ping' sends to Redis)\n"
                  << ansi_cyan() << "  timeout <ms>" << ansi_reset() << "       Set command timeout\n"
                  << ansi_cyan() << "  reconnect" << ansi_reset() << "            Toggle auto-reconnect\n"
                  << ansi_cyan() << "  db <n>" << ansi_reset() << "             Select database\n"
                  << ansi_cyan() << "  history [n]" << ansi_reset() << "      Show command history (last n entries)\n"
                  << "\n"
                  << ansi_bold() << "Pipeline Commands (\\ prefix required in pipeline mode):" << ansi_reset() << "\n"
                  << ansi_cyan() << "  \\exec, \\e" << ansi_reset() << "          Execute pipeline\n"
                  << ansi_cyan() << "  \\abort, \\a" << ansi_reset() << "         Abort pipeline\n"
                  << "\n"
                  << ansi_bold() << "Analysis & Export Commands:" << ansi_reset() << "\n"
                  << ansi_cyan() << "  scan-keys [pattern] [count]" << ansi_reset() << "\n"
                  << ansi_cyan() << "  key-info <key>" << ansi_reset() << "\n"
                  << ansi_cyan() << "  export [pattern] [file]" << ansi_reset() << "\n"
                  << "\n"
                  << ansi_bold() << "Redis Commands:" << ansi_reset() << "\n"
                  << ansi_gray() << "  Any Redis command: SET key value, GET key, HGETALL key, etc." << ansi_reset() << "\n"
                  << ansi_gray() << "  MULTI enters transaction mode; EXEC/DISCARD exits it." << ansi_reset() << "\n"
                  << ansi_gray() << "  SUBSCRIBE/PSUBSCRIBE enters subscribe mode; UNSUBSCRIBE exits." << ansi_reset() << "\n"
                  << ansi_gray() << "  Supports quoted arguments: GET \"my key\"" << ansi_reset() << "\n"
                  << ansi_gray() << "  Tab-completion and command history available in interactive mode." << ansi_reset() << "\n\n";
    }

    std::string build_prompt(const ReplState &state)
    {
        if (state.pipeline_mode)
        {
            return std::string(ansi_yellow()) + "pipe> " + ansi_reset();
        }

        if (!state.connected)
        {
            return std::string(ansi_gray()) + "not-connected> " + ansi_reset();
        }

        std::ostringstream oss;
        oss << ansi_green() << state.opt.host_ << ":" << state.opt.port_ << ansi_reset()
            << ansi_gray() << "[" << state.opt.db_ << "]";

        if (state.multi_mode)
        {
            oss << ansi_yellow() << " (MULTI)";
        }

        if (state.subscribe_mode)
        {
            oss << ansi_cyan() << " (subscribed)";
        }

        oss << ansi_gray() << "> " << ansi_reset();
        return oss.str();
    }

    static bool do_connect(ReplState &state)
    {
        if (state.connected)
        {
            std::cout << ansi_yellow() << "Already connected. Use \\disconnect first." << ansi_reset() << "\n";
            return true;
        }

        state.client = std::make_shared<yuan::redis::RedisClient>(state.opt);
        bool ok = state.client->ensure_connected();
        if (!ok)
        {
            std::cout << ansi_red() << "Connection failed: " << state.opt.host_ << ":" << state.opt.port_ << ansi_reset() << "\n";
            state.client->close();
            state.client.reset();
            return false;
        }

        state.connected = true;
        std::cout << ansi_green() << "Connected to " << state.opt.host_ << ":" << state.opt.port_
                  << " db=" << state.opt.db_ << ansi_reset() << "\n";

        if (state.opt.db_ > 0)
        {
            auto result = state.client->select(state.opt.db_);
            if (result && !result->as<yuan::redis::ErrorValue>())
            {
                std::cout << ansi_green() << "Database " << state.opt.db_ << " selected." << ansi_reset() << "\n";
            }
            else
            {
                std::cout << ansi_red() << "Failed to select db " << state.opt.db_ << ": " << format_value(result, state.format_style) << ansi_reset() << "\n";
            }
        }

        return true;
    }

    static void do_disconnect(ReplState &state)
    {
        if (!state.connected)
        {
            std::cout << ansi_gray() << "Not connected." << ansi_reset() << "\n";
            return;
        }

        if (state.pipeline_mode)
        {
            state.pipeline_cmds.clear();
            state.pipeline_mode = false;
        }

        state.client->close();
        state.client.reset();
        state.connected = false;
        std::cout << ansi_yellow() << "Disconnected." << ansi_reset() << "\n";
    }

    static void enter_subscribe_mode(ReplState &state, const std::vector<std::string> &channels)
    {
        if (!state.connected)
        {
            std::cout << ansi_red() << "Not connected. Use connect first." << ansi_reset() << "\n";
            return;
        }

        if (channels.empty())
        {
            std::cout << ansi_red() << "Usage: SUBSCRIBE channel [channel ...]" << ansi_reset() << "\n";
            return;
        }

        auto msg_callback = [](const std::vector<yuan::redis::SubMessage> &messages)
        {
            for (const auto &msg : messages)
            {
                std::string channel = msg.channel ? msg.channel->to_string() : "?";
                std::string data = msg.message ? msg.message->to_string() : "?";
                std::cout << ansi_cyan() << channel << ansi_reset() << ": " << data << "\n";
            }
        };

        auto result = state.client->subscribe(channels, msg_callback);
        std::cout << format_value(result, state.format_style);

        state.subscribe_mode = true;
        std::cout << ansi_yellow() << "Subscribe mode. Type unsubscribe to exit, Ctrl-C to abort." << ansi_reset() << "\n";

        g_interrupted = 0;
        auto old_handler = std::signal(SIGINT, sigint_handler);

        while (state.subscribe_mode && state.client->is_subscribing() && !g_interrupted)
        {
            int r = state.client->receive(100);
            if (r < 0 && !state.client->is_subscribing())
            {
                break;
            }

            if (stdin_has_data())
            {
                std::cout << build_prompt(state) << std::flush;
                std::string line;
                if (!std::getline(std::cin, line)) break;

                auto tokens = tokenize(line);
                if (tokens.empty()) continue;

                auto lower = to_lower(tokens[0]);

                if (lower == "unsubscribe")
                {
                    std::vector<std::string> unsubs(tokens.begin() + 1, tokens.end());
                    auto res = state.client->unsubscribe(unsubs);
                    std::cout << format_value(res, state.format_style);
                    if (!state.client->is_subscribing())
                    {
                        state.subscribe_mode = false;
                    }
                }
                else if (lower == "punsubscribe")
                {
                    std::vector<std::string> unsubs(tokens.begin() + 1, tokens.end());
                    auto res = state.client->punsubscribe(unsubs);
                    std::cout << format_value(res, state.format_style);
                    if (!state.client->is_subscribing())
                    {
                        state.subscribe_mode = false;
                    }
                }
                else if (lower == "subscribe")
                {
                    std::vector<std::string> new_ch(tokens.begin() + 1, tokens.end());
                    if (!new_ch.empty())
                    {
                        auto res = state.client->subscribe(new_ch, nullptr);
                        std::cout << format_value(res, state.format_style);
                    }
                }
                else if (lower == "psubscribe")
                {
                    std::vector<std::string> new_pat(tokens.begin() + 1, tokens.end());
                    if (!new_pat.empty())
                    {
                        auto res = state.client->psubscribe(new_pat, nullptr);
                        std::cout << format_value(res, state.format_style);
                    }
                }
                else if (lower == "quit" || lower == "exit")
                {
                    state.client->unsubscribe({});
                    state.client->punsubscribe({});
                    state.subscribe_mode = false;
                    state.should_exit = true;
                }
                else
                {
                    std::cout << ansi_yellow() << "In subscribe mode. Only subscribe/unsubscribe/quit allowed." << ansi_reset() << "\n";
                }
            }
        }

        if (g_interrupted)
        {
            std::cout << "\n" << ansi_yellow() << "Interrupted. Unsubscribing..." << ansi_reset() << "\n";
            state.client->unsubscribe({});
            state.client->punsubscribe({});
            state.subscribe_mode = false;
            g_interrupted = 0;
        }

        std::signal(SIGINT, old_handler);
        state.subscribe_mode = false;
    }

    static void enter_psubscribe_mode(ReplState &state, const std::vector<std::string> &patterns)
    {
        if (!state.connected)
        {
            std::cout << ansi_red() << "Not connected. Use connect first." << ansi_reset() << "\n";
            return;
        }

        if (patterns.empty())
        {
            std::cout << ansi_red() << "Usage: PSUBSCRIBE pattern [pattern ...]" << ansi_reset() << "\n";
            return;
        }

        auto pmsg_callback = [](const std::vector<yuan::redis::PSubMessage> &messages)
        {
            for (const auto &msg : messages)
            {
                std::string pattern = msg.pattern ? msg.pattern->to_string() : "?";
                std::string channel = msg.channel ? msg.channel->to_string() : "?";
                std::string data = msg.message ? msg.message->to_string() : "?";
                std::cout << ansi_magenta() << pattern << " " << ansi_reset()
                          << ansi_cyan() << channel << ansi_reset() << ": " << data << "\n";
            }
        };

        auto result = state.client->psubscribe(patterns, pmsg_callback);
        std::cout << format_value(result, state.format_style);

        state.subscribe_mode = true;
        std::cout << ansi_yellow() << "Subscribe mode. Type punsubscribe to exit, Ctrl-C to abort." << ansi_reset() << "\n";

        g_interrupted = 0;
        auto old_handler = std::signal(SIGINT, sigint_handler);

        while (state.subscribe_mode && state.client->is_subscribing() && !g_interrupted)
        {
            int r = state.client->receive(100);
            if (r < 0 && !state.client->is_subscribing())
            {
                break;
            }

            if (stdin_has_data())
            {
                std::cout << build_prompt(state) << std::flush;
                std::string line;
                if (!std::getline(std::cin, line)) break;

                auto tokens = tokenize(line);
                if (tokens.empty()) continue;

                auto lower = to_lower(tokens[0]);

                if (lower == "unsubscribe")
                {
                    std::vector<std::string> unsubs(tokens.begin() + 1, tokens.end());
                    auto res = state.client->unsubscribe(unsubs);
                    std::cout << format_value(res, state.format_style);
                    if (!state.client->is_subscribing()) state.subscribe_mode = false;
                }
                else if (lower == "punsubscribe")
                {
                    std::vector<std::string> unsubs(tokens.begin() + 1, tokens.end());
                    auto res = state.client->punsubscribe(unsubs);
                    std::cout << format_value(res, state.format_style);
                    if (!state.client->is_subscribing()) state.subscribe_mode = false;
                }
                else if (lower == "subscribe")
                {
                    std::vector<std::string> new_ch(tokens.begin() + 1, tokens.end());
                    if (!new_ch.empty())
                    {
                        auto res = state.client->subscribe(new_ch, nullptr);
                        std::cout << format_value(res, state.format_style);
                    }
                }
                else if (lower == "psubscribe")
                {
                    std::vector<std::string> new_pat(tokens.begin() + 1, tokens.end());
                    if (!new_pat.empty())
                    {
                        auto res = state.client->psubscribe(new_pat, nullptr);
                        std::cout << format_value(res, state.format_style);
                    }
                }
                else if (lower == "quit" || lower == "exit")
                {
                    state.client->unsubscribe({});
                    state.client->punsubscribe({});
                    state.subscribe_mode = false;
                    state.should_exit = true;
                }
                else
                {
                    std::cout << ansi_yellow() << "In subscribe mode. Only subscribe/unsubscribe/quit allowed." << ansi_reset() << "\n";
                }
            }
        }

        if (g_interrupted)
        {
            std::cout << "\n" << ansi_yellow() << "Interrupted. Unsubscribing..." << ansi_reset() << "\n";
            state.client->unsubscribe({});
            state.client->punsubscribe({});
            state.subscribe_mode = false;
            g_interrupted = 0;
        }

        std::signal(SIGINT, old_handler);
        state.subscribe_mode = false;
    }

    void handle_meta_command(ReplState &state, const std::string &line)
    {
        auto tokens = tokenize(line);
        if (tokens.empty()) return;

        auto cmd = normalize_meta(tokens[0]);

        if (cmd == "help" || cmd == "h")
        {
            print_help();
            return;
        }

        if (cmd == "quit" || cmd == "q" || cmd == "exit")
        {
            state.should_exit = true;
            return;
        }

        if (cmd == "connect" || cmd == "c")
        {
            if (tokens.size() >= 2)
            {
                std::string host_port = tokens[1];
                auto pos = host_port.find(':');
                if (pos != std::string::npos)
                {
                    state.opt.host_ = host_port.substr(0, pos);
                    state.opt.port_ = safe_stoi(host_port.substr(pos + 1), state.opt.port_);
                }
                else
                {
                    state.opt.host_ = host_port;
                }
            }

            if (tokens.size() >= 3)
            {
                state.opt.port_ = safe_stoi(tokens[2], state.opt.port_);
            }

            do_connect(state);
            return;
        }

        if (cmd == "disconnect" || cmd == "d")
        {
            do_disconnect(state);
            return;
        }

        if (cmd == "status" || cmd == "s")
        {
            if (!state.connected)
            {
                std::cout << ansi_gray() << "Not connected." << ansi_reset() << "\n";
            }
            else
            {
                auto s = state.client->stats();
                std::cout << ansi_bold() << "Connection Status:" << ansi_reset() << "\n"
                          << "  host:       " << state.opt.host_ << ":" << state.opt.port_ << "\n"
                          << "  db:         " << state.opt.db_ << "\n"
                          << "  connected:  " << (s.connected ? std::string(ansi_green()) + "yes" : std::string(ansi_red()) + "no") << ansi_reset() << "\n"
                          << "  closed:     " << (s.closed ? "yes" : "no") << "\n"
                          << "  in_flight:  " << s.in_flight << "\n"
                          << "  reconnects: " << s.reconnect_attempts << " (success: " << s.reconnect_successes << ")" << "\n"
                          << "  timeouts:   " << s.command_timeouts << "\n"
                          << "  errors:     " << s.command_errors << "\n";
            }
            return;
        }

        if (cmd == "stats")
        {
            if (!state.connected)
            {
                std::cout << ansi_gray() << "Not connected." << ansi_reset() << "\n";
            }
            else
            {
                auto s = state.client->stats();
                std::cout << ansi_bold() << "Client Stats:" << ansi_reset() << "\n"
                          << "  commands_total:    " << s.commands_total << "\n"
                          << "  command_errors:    " << s.command_errors << "\n"
                          << "  command_timeouts:  " << s.command_timeouts << "\n"
                          << "  avg_latency_us:    " << std::fixed << std::setprecision(1) << s.avg_latency_us() << "\n"
                          << "  total_latency_us:  " << s.total_latency_us << "\n"
                          << "  reconnect_attempts: " << s.reconnect_attempts << "\n"
                          << "  reconnect_successes: " << s.reconnect_successes << "\n"
                          << "  health_checks:     " << s.health_checks << "\n"
                          << "  hc_successes:      " << s.health_check_successes << "\n"
                          << "  hc_failures:       " << s.health_check_failures << "\n";
            }
            return;
        }

        if (cmd == "format" || cmd == "f")
        {
            if (tokens.size() >= 2)
            {
                auto style = to_lower(tokens[1]);
                if (style == "raw")
                {
                    state.format_style = FormatStyle::raw;
                    std::cout << ansi_green() << "Output format: raw" << ansi_reset() << "\n";
                }
                else if (style == "pretty")
                {
                    state.format_style = FormatStyle::pretty;
                    std::cout << ansi_green() << "Output format: pretty" << ansi_reset() << "\n";
                }
                else
                {
                    std::cout << ansi_red() << "Unknown format: " << tokens[1] << " (use: pretty|raw)" << ansi_reset() << "\n";
                }
            }
            else
            {
                std::cout << "Current format: " << (state.format_style == FormatStyle::pretty ? "pretty" : "raw") << "\n";
            }
            return;
        }

        if (cmd == "pipeline" || cmd == "p")
        {
            if (!state.connected)
            {
                std::cout << ansi_red() << "Not connected. Use connect first." << ansi_reset() << "\n";
                return;
            }

            state.pipeline_mode = true;
            state.pipeline_cmds.clear();
            std::cout << ansi_yellow() << "Pipeline mode. Enter Redis commands, then exec to run or abort to cancel." << ansi_reset() << "\n";
            return;
        }

        if (cmd == "exec" || cmd == "e")
        {
            if (!state.pipeline_mode)
            {
                std::cout << ansi_red() << "Not in pipeline mode. Use \\exec only in pipeline mode, or EXEC for transactions." << ansi_reset() << "\n";
                return;
            }

            if (state.pipeline_cmds.empty())
            {
                std::cout << ansi_yellow() << "Pipeline is empty." << ansi_reset() << "\n";
                state.pipeline_mode = false;
                return;
            }

            auto start = std::chrono::steady_clock::now();
            auto result = state.client->pipeline(state.pipeline_cmds);
            auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start).count();

            std::cout << ansi_gray() << "(" << state.pipeline_cmds.size() << " commands, " << elapsed_ms << "ms)" << ansi_reset() << "\n";
            std::cout << format_value(result, state.format_style);

            state.pipeline_cmds.clear();
            state.pipeline_mode = false;
            return;
        }

        if (cmd == "abort" || cmd == "a")
        {
            if (!state.pipeline_mode)
            {
                std::cout << ansi_gray() << "Not in pipeline mode." << ansi_reset() << "\n";
                return;
            }

            state.pipeline_cmds.clear();
            state.pipeline_mode = false;
            std::cout << ansi_yellow() << "Pipeline aborted." << ansi_reset() << "\n";
            return;
        }

        if (cmd == "ping")
        {
            if (!state.connected)
            {
                std::cout << ansi_red() << "Not connected." << ansi_reset() << "\n";
                return;
            }

            auto result = state.client->ping();
            std::cout << format_value(result, state.format_style);
            return;
        }

        if (cmd == "timeout")
        {
            if (tokens.size() >= 2)
            {
                int ms = safe_stoi(tokens[1], state.opt.command_timeout_ms_);
                state.opt.command_timeout_ms_ = ms;
                if (state.connected)
                {
                    state.client->set_option(state.opt);
                }
                std::cout << ansi_green() << "Command timeout set to " << ms << "ms" << ansi_reset() << "\n";
            }
            else
            {
                std::cout << "Current timeout: " << state.opt.command_timeout_ms_ << "ms\n";
            }
            return;
        }

        if (cmd == "reconnect")
        {
            if (tokens.size() >= 2)
            {
                state.opt.reconnect_ = (to_lower(tokens[1]) == "on" || tokens[1] == "1");
                if (state.connected)
                {
                    state.client->set_option(state.opt);
                }
                std::cout << ansi_green() << "Auto-reconnect: " << (state.opt.reconnect_ ? "on" : "off") << ansi_reset() << "\n";
            }
            else
            {
                std::cout << "Auto-reconnect: " << (state.opt.reconnect_ ? "on" : "off") << "\n";
            }
            return;
        }

        if (cmd == "db")
        {
            if (tokens.size() >= 2)
            {
                int db = safe_stoi(tokens[1], state.opt.db_);
                state.opt.db_ = db;
                if (state.connected)
                {
                    auto result = state.client->select(db);
                    std::cout << format_value(result, state.format_style);
                }
            }
            else
            {
                std::cout << "Current db: " << state.opt.db_ << "\n";
            }
            return;
        }

        if (cmd == "history")
        {
            int n = 20;
            if (tokens.size() >= 2)
            {
                n = safe_stoi(tokens[1], 20);
            }
            print_history(n);
            return;
        }

        if (cmd == "scan-keys")
        {
            if (!state.connected)
            {
                std::cout << ansi_red() << "Not connected." << ansi_reset() << "\n";
                return;
            }

            std::string pattern = tokens.size() >= 2 ? tokens[1] : "*";
            int64_t count = tokens.size() >= 3 ? safe_stoi(tokens[2], 100) : 100;
            int64_t cursor = 0;
            int total = 0;

            std::cout << ansi_bold() << "Scanning keys matching: " << pattern << ansi_reset() << "\n";

            do
            {
                auto result = state.client->scan(cursor, pattern, count);
                if (!result)
                {
                    std::cout << ansi_red() << "Scan failed." << ansi_reset() << "\n";
                    return;
                }

                auto arr = result->as<yuan::redis::ArrayValue>();
                if (!arr)
                {
                    std::cout << format_value(result, state.format_style);
                    return;
                }

                const auto &items = arr->get_values();
                if (items.size() >= 2)
                {
                    auto cursor_val = items[0] ? items[0]->as<yuan::redis::StringValue>() : nullptr;
                    if (cursor_val)
                    {
                        try { cursor = std::stoll(cursor_val->get_value()); }
                        catch (...) { cursor = 0; }
                    }

                    auto keys_arr = items[1] ? items[1]->as<yuan::redis::ArrayValue>() : nullptr;
                    if (keys_arr)
                    {
                        for (const auto &key_val : keys_arr->get_values())
                        {
                            if (key_val)
                            {
                                ++total;
                                std::cout << ansi_gray() << std::setw(5) << total << ") " << ansi_reset()
                                          << key_val->to_string() << "\n";
                            }
                        }
                    }
                }
                else
                {
                    std::cout << format_value(result, state.format_style);
                    return;
                }
            } while (cursor != 0);

            std::cout << ansi_green() << "Total: " << total << " key(s)" << ansi_reset() << "\n";
            return;
        }

        if (cmd == "key-info")
        {
            if (!state.connected)
            {
                std::cout << ansi_red() << "Not connected." << ansi_reset() << "\n";
                return;
            }

            if (tokens.size() < 2)
            {
                std::cout << ansi_red() << "Usage: key-info <key>" << ansi_reset() << "\n";
                return;
            }

            const std::string &key = tokens[1];

            auto type_result = state.client->key_type(key);
            if (!type_result)
            {
                std::cout << ansi_red() << "Failed to get key info." << ansi_reset() << "\n";
                return;
            }

            auto err = type_result->as<yuan::redis::ErrorValue>();
            if (err)
            {
                std::cout << ansi_red() << "(error) " << err->to_string() << ansi_reset() << "\n";
                return;
            }

            std::string key_type = type_result->get_type() == yuan::redis::resp_status
                ? type_result->get_raw_str()
                : type_result->to_string();
            if (key_type == "none")
            {
                std::cout << ansi_yellow() << "Key \"" << key << "\" does not exist." << ansi_reset() << "\n";
                return;
            }

            auto ttl_result = state.client->ttl(key);
            auto encoding_result = state.client->command("OBJECT", {"ENCODING", key});
            auto memory_result = state.client->command("MEMORY", {"USAGE", key});

            std::cout << ansi_bold() << "Key Info: " << ansi_reset() << key << "\n"
                      << "  type:     " << ansi_cyan() << key_type << ansi_reset() << "\n"
                      << "  ttl:      ";

            if (ttl_result)
            {
                auto iv = ttl_result->as<yuan::redis::IntValue>();
                if (iv)
                {
                    int64_t ttl = iv->get_value();
                    if (ttl == -1) std::cout << ansi_gray() << "no expiry" << ansi_reset();
                    else if (ttl == -2) std::cout << ansi_red() << "key does not exist" << ansi_reset();
                    else std::cout << ansi_green() << ttl << "s" << ansi_reset();
                }
                else std::cout << format_value(ttl_result, state.format_style);
            }
            std::cout << "\n";

            std::cout << "  encoding: ";
            if (encoding_result) std::cout << ansi_cyan() << encoding_result->to_string() << ansi_reset();
            std::cout << "\n";

            std::cout << "  memory:   ";
            if (memory_result)
            {
                auto miv = memory_result->as<yuan::redis::IntValue>();
                if (miv) std::cout << ansi_green() << miv->get_value() << " bytes" << ansi_reset();
                else std::cout << format_value(memory_result, state.format_style);
            }
            std::cout << "\n";

            if (key_type == "hash")
            {
                auto hlen = state.client->command("HLEN", {key});
                std::cout << "  fields:   ";
                if (hlen)
                {
                    auto iv = hlen->as<yuan::redis::IntValue>();
                    if (iv) std::cout << iv->get_value();
                    else std::cout << format_value(hlen, state.format_style);
                }
                std::cout << "\n";
            }
            else if (key_type == "list")
            {
                auto llen = state.client->command("LLEN", {key});
                std::cout << "  length:   ";
                if (llen)
                {
                    auto iv = llen->as<yuan::redis::IntValue>();
                    if (iv) std::cout << iv->get_value();
                    else std::cout << format_value(llen, state.format_style);
                }
                std::cout << "\n";
            }
            else if (key_type == "set")
            {
                auto scard = state.client->command("SCARD", {key});
                std::cout << "  members:  ";
                if (scard)
                {
                    auto iv = scard->as<yuan::redis::IntValue>();
                    if (iv) std::cout << iv->get_value();
                    else std::cout << format_value(scard, state.format_style);
                }
                std::cout << "\n";
            }
            else if (key_type == "zset")
            {
                auto zcard = state.client->command("ZCARD", {key});
                std::cout << "  members:  ";
                if (zcard)
                {
                    auto iv = zcard->as<yuan::redis::IntValue>();
                    if (iv) std::cout << iv->get_value();
                    else std::cout << format_value(zcard, state.format_style);
                }
                std::cout << "\n";
            }
            else if (key_type == "stream")
            {
                auto xlen = state.client->command("XLEN", {key});
                std::cout << "  length:   ";
                if (xlen)
                {
                    auto iv = xlen->as<yuan::redis::IntValue>();
                    if (iv) std::cout << iv->get_value();
                    else std::cout << format_value(xlen, state.format_style);
                }
                std::cout << "\n";
            }

            return;
        }

        if (cmd == "export")
        {
            if (!state.connected)
            {
                std::cout << ansi_red() << "Not connected." << ansi_reset() << "\n";
                return;
            }

            std::string pattern = tokens.size() >= 2 ? tokens[1] : "*";
            std::string filepath = tokens.size() >= 3 ? tokens[2] : "yredis_export.json";

            int64_t cursor = 0;
            int total = 0;
            int errors = 0;
            std::ostringstream json;
            json << "{\n  \"keys\": [\n";
            bool first = true;

            std::cout << ansi_bold() << "Exporting keys matching: " << pattern
                      << " -> " << filepath << ansi_reset() << "\n";

            do
            {
                auto result = state.client->scan(cursor, pattern, 100);
                if (!result)
                {
                    std::cout << ansi_red() << "Scan failed." << ansi_reset() << "\n";
                    return;
                }

                auto arr = result->as<yuan::redis::ArrayValue>();
                if (!arr) break;

                const auto &items = arr->get_values();
                if (items.size() >= 2)
                {
                    auto cursor_val = items[0] ? items[0]->as<yuan::redis::StringValue>() : nullptr;
                    if (cursor_val)
                    {
                        try { cursor = std::stoll(cursor_val->get_value()); }
                        catch (...) { cursor = 0; }
                    }

                    auto keys_arr = items[1] ? items[1]->as<yuan::redis::ArrayValue>() : nullptr;
                    if (keys_arr)
                    {
                        for (const auto &key_val : keys_arr->get_values())
                        {
                            if (!key_val) continue;
                            std::string key = key_val->to_string();

                            auto type_result = state.client->key_type(key);
                            if (!type_result) { ++errors; continue; }
            std::string key_type = type_result->get_type() == yuan::redis::resp_status
                ? type_result->get_raw_str()
                : type_result->to_string();
                            if (key_type == "none") continue;

                            auto ttl_result = state.client->ttl(key);
                            int64_t ttl = -1;
                            if (ttl_result)
                            {
                                auto iv = ttl_result->as<yuan::redis::IntValue>();
                                if (iv) ttl = iv->get_value();
                            }

                            if (!first) json << ",\n";
                            first = false;

                            json << "    {\"key\": \"";
                            for (char c : key)
                            {
                                if (c == '"' || c == '\\') json << '\\';
                                json << c;
                            }
                            json << "\", \"type\": \"" << key_type << "\""
                                 << ", \"ttl\": " << ttl << "}";

                            ++total;
                        }
                    }
                }
            } while (cursor != 0);

            json << "\n  ]\n}\n";

            std::ofstream ofs(filepath);
            if (!ofs)
            {
                std::cout << ansi_red() << "Failed to open file: " << filepath << ansi_reset() << "\n";
                return;
            }
            ofs << json.str();

            std::cout << ansi_green() << "Exported " << total << " key(s)"
                      << (errors > 0 ? " (" + std::to_string(errors) + " errors)" : "")
                      << " to " << filepath << ansi_reset() << "\n";
            return;
        }

        std::cout << ansi_red() << "Unknown command: " << tokens[0] << ansi_reset() << "\n";
    }

    std::shared_ptr<yuan::redis::RedisValue> execute_redis_command(
        ReplState &state,
        const std::string &name,
        const std::vector<std::string> &args)
    {
        if (!state.connected)
        {
            do_connect(state);
            if (!state.connected)
            {
                return yuan::redis::ErrorValue::from_string("not connected");
            }
        }

        auto start = std::chrono::steady_clock::now();
        auto result = state.client->command(name, args);
        auto elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - start).count();

        if (elapsed_us >= 1000000)
        {
            double sec = elapsed_us / 1000000.0;
            std::cout << ansi_gray() << "(" << std::fixed << std::setprecision(2) << sec << "s)" << ansi_reset() << "\n";
        }
        else if (elapsed_us >= 1000)
        {
            double ms = elapsed_us / 1000.0;
            std::cout << ansi_gray() << "(" << std::fixed << std::setprecision(1) << ms << "ms)" << ansi_reset() << "\n";
        }
        else
        {
            std::cout << ansi_gray() << "(" << elapsed_us << "\u03bcs)" << ansi_reset() << "\n";
        }

        return result;
    }

    static void handle_redis_command_in_pipeline(ReplState &state, const std::string &name, const std::vector<std::string> &args)
    {
        state.pipeline_cmds.emplace_back(name, args);
        std::cout << ansi_gray() << "+ [" << state.pipeline_cmds.size() << "] " << name << ansi_reset() << "\n";
    }

    void repl_loop(ReplState &state)
    {
        input_init(state);

        while (true)
        {
            if (state.should_exit)
            {
                std::cout << ansi_cyan() << "Bye!" << ansi_reset() << "\n";
                break;
            }

            std::string prompt = build_prompt(state);
            auto rr = read_line(prompt);
            if (rr.eof)
            {
                std::cout << ansi_cyan() << "Bye!" << ansi_reset() << "\n";
                break;
            }

            if (rr.line.empty()) continue;

            auto tokens = tokenize(rr.line);
            if (tokens.empty()) continue;

            auto lower_first = to_lower(tokens[0]);
            std::vector<std::string> args(tokens.begin() + 1, tokens.end());

            if (lower_first == "subscribe")
            {
                enter_subscribe_mode(state, args);
                continue;
            }

            if (lower_first == "psubscribe")
            {
                enter_psubscribe_mode(state, args);
                continue;
            }

            if (lower_first == "multi")
            {
                if (!state.connected)
                {
                    do_connect(state);
                    if (!state.connected) continue;
                }
                bool ok = state.client->multi();
                if (ok)
                {
                    state.multi_mode = true;
                    std::cout << ansi_green() << "OK" << ansi_reset() << "\n";
                }
                else
                {
                    std::cout << ansi_red() << "(error) MULTI calls can not be nested" << ansi_reset() << "\n";
                }
                continue;
            }

            if (state.multi_mode && lower_first == "exec")
            {
                auto result = state.client->exec();
                std::cout << format_value(result, state.format_style);
                state.multi_mode = false;
                continue;
            }

            if (state.multi_mode && lower_first == "discard")
            {
                auto result = state.client->discard();
                std::cout << format_value(result, state.format_style);
                state.multi_mode = false;
                continue;
            }

            if (is_meta_command(tokens[0]))
            {
                handle_meta_command(state, rr.line);
                continue;
            }

            std::string cmd_name = tokens[0];

            if (state.pipeline_mode)
            {
                handle_redis_command_in_pipeline(state, cmd_name, args);
                continue;
            }

            auto result = execute_redis_command(state, cmd_name, args);

            if (state.multi_mode && !result)
            {
                std::cout << ansi_green() << "QUEUED" << ansi_reset() << "\n";
                continue;
            }

            std::cout << format_value(result, state.format_style);

            auto err = result ? result->as<yuan::redis::ErrorValue>() : nullptr;
            if (err)
            {
                auto msg = err->to_string();
                if (msg.find("MOVED") != std::string::npos ||
                    msg.find("ASK") != std::string::npos ||
                    msg.find("NOAUTH") != std::string::npos)
                {
                    std::cout << ansi_yellow() << "Hint: this may require cluster mode or authentication." << ansi_reset() << "\n";
                }
            }
        }

        if (state.connected)
        {
            state.client->close();
        }

        input_cleanup();
    }
}