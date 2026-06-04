#include "input.h"
#include "repl.h"
#include "formatter.h"

#include <algorithm>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#if defined(HAVE_READLINE) || defined(HAVE_EDITLINE)
#include <readline/readline.h>
#include <readline/history.h>
#endif

namespace yredis
{
    static const int max_history_entries = 2000;

    static ReplState *g_state_for_completion = nullptr;

    static const std::vector<std::string> meta_commands = {
        "\\help", "\\h",
        "\\quit", "\\q",
        "\\connect", "\\c",
        "\\disconnect", "\\d",
        "\\status", "\\s",
        "\\stats",
        "\\format", "\\f",
        "\\pipeline", "\\p",
        "\\exec", "\\e",
        "\\abort", "\\a",
        "\\ping",
        "\\timeout",
        "\\reconnect",
        "\\db",
        "\\history",
        "\\scan-keys",
        "\\key-info",
        "\\export",
    };

    static const std::vector<std::string> bare_meta_commands = {
        "help", "h",
        "quit", "q", "exit",
        "connect", "c",
        "disconnect", "d",
        "status", "s",
        "stats",
        "format", "f",
        "pipeline", "p",
        "timeout",
        "reconnect",
        "db",
        "history",
        "scan-keys",
        "key-info",
        "export",
    };

    static const std::vector<std::string> redis_commands = {
        "APPEND",
        "AUTH",
        "BGREWRITEAOF",
        "BGSAVE",
        "BITCOUNT",
        "BITFIELD",
        "BITOP",
        "BITPOS",
        "BLMOVE",
        "BLMPOP",
        "BLPOP",
        "BRPOP",
        "BRPOPLPUSH",
        "BZMPOP",
        "BZPOPMAX",
        "BZPOPMIN",
        "CLIENT",
        "CLUSTER",
        "COMMAND",
        "CONFIG",
        "COPY",
        "DBSIZE",
        "DECR",
        "DECRBY",
        "DEL",
        "DISCARD",
        "DUMP",
        "ECHO",
        "EVAL",
        "EVALSHA",
        "EXISTS",
        "EXPIRE",
        "EXPIREAT",
        "EXPIRETIME",
        "FLUSHALL",
        "FLUSHDB",
        "GEOADD",
        "GEODIST",
        "GEOHASH",
        "GEOPOS",
        "GEOSEARCH",
        "GET",
        "GETDEL",
        "GETEX",
        "GETSET",
        "HDEL",
        "HELLO",
        "HEXISTS",
        "HGET",
        "HGETALL",
        "HINCRBY",
        "HINCRBYFLOAT",
        "HKEYS",
        "HLEN",
        "HMGET",
        "HMSET",
        "HRANDFIELD",
        "HSCAN",
        "HSET",
        "HSETNX",
        "HVALS",
        "INCR",
        "INCRBY",
        "INCRBYFLOAT",
        "INFO",
        "KEYS",
        "LINDEX",
        "LINSERT",
        "LLEN",
        "LMOVE",
        "LMPOP",
        "LPOP",
        "LPOS",
        "LPUSH",
        "LPUSHX",
        "LRANGE",
        "LREM",
        "LSET",
        "LTRIM",
        "MEMORY",
        "MGET",
        "MIGRATE",
        "MODULE",
        "MONITOR",
        "MOVE",
        "MSET",
        "MSETNX",
        "MULTI",
        "OBJECT",
        "PERSIST",
        "PEXPIRE",
        "PEXPIREAT",
        "PEXPIRETIME",
        "PFADD",
        "PFCOUNT",
        "PFMERGE",
        "PING",
        "PSETEX",
        "PSUBSCRIBE",
        "PTTL",
        "PUBLISH",
        "PUBSUB",
        "PUNSUBSCRIBE",
        "QUIT",
        "RANDOMKEY",
        "RENAME",
        "RENAMENX",
        "RESET",
        "RESTORE",
        "RPOP",
        "RPOPLPUSH",
        "RPUSH",
        "RPUSHX",
        "SADD",
        "SCAN",
        "SCARD",
        "SDIFF",
        "SDIFFSTORE",
        "SELECT",
        "SET",
        "SETBIT",
        "SETEX",
        "SETNX",
        "SETRANGE",
        "SHUTDOWN",
        "SINTER",
        "SINTERSTORE",
        "SISMEMBER",
        "SMISMEMBER",
        "SMOVE",
        "SORT",
        "SPOP",
        "SRANDMEMBER",
        "SREM",
        "SSUBSCRIBE",
        "SUBSCRIBE",
        "SUBSTR",
        "SUNION",
        "SUNIONSTORE",
        "SUNSUBSCRIBE",
        "SWAPDB",
        "TIME",
        "TOUCH",
        "TTL",
        "TYPE",
        "UNLINK",
        "UNSUBSCRIBE",
        "UNWATCH",
        "WAIT",
        "WATCH",
        "XACK",
        "XADD",
        "XCLAIM",
        "XDEL",
        "XGROUP",
        "XINFO",
        "XLEN",
        "XPENDING",
        "XRANGE",
        "XREAD",
        "XREADGROUP",
        "XREVRANGE",
        "XTRIM",
        "ZADD",
        "ZCARD",
        "ZCOUNT",
        "ZDIFF",
        "ZDIFFSTORE",
        "ZINCRBY",
        "ZINTER",
        "ZINTERSTORE",
        "ZLEXCOUNT",
        "ZMPOP",
        "ZMSCORE",
        "ZPOPMAX",
        "ZPOPMIN",
        "ZRANDMEMBER",
        "ZRANGE",
        "ZRANGEBYLEX",
        "ZRANGEBYSCORE",
        "ZRANK",
        "ZREM",
        "ZREMRANGEBYLEX",
        "ZREMRANGEBYRANK",
        "ZREMRANGEBYSCORE",
        "ZREVRANGE",
        "ZREVRANGEBYLEX",
        "ZREVRANGEBYSCORE",
        "ZREVRANK",
        "ZSCAN",
        "ZSCORE",
        "ZUNION",
        "ZUNIONSTORE",
    };

    static std::string get_history_path()
    {
#ifdef _WIN32
        const char *home = getenv("USERPROFILE");
        if (!home || home[0] == '\0') home = getenv("APPDATA");
        if (!home || home[0] == '\0') home = ".";
#else
        const char *home = getenv("HOME");
        if (!home || home[0] == '\0') home = ".";
#endif
        return std::string(home) + "/.yredis_history";
    }

#if defined(HAVE_READLINE) || defined(HAVE_EDITLINE)

    static char *completion_generator(const char *text, int state)
    {
        static std::vector<std::string> matches;
        static size_t match_index = 0;

        if (state == 0)
        {
            matches.clear();
            match_index = 0;

            std::string prefix(text);
            bool is_meta = !prefix.empty() && prefix[0] == '\\';

            if (is_meta)
            {
                for (const auto &cmd : meta_commands)
                {
                    if (cmd.compare(0, prefix.size(), prefix) == 0)
                    {
                        matches.push_back(cmd);
                    }
                }
            }
            else
            {
                std::string upper_prefix = prefix;
                std::transform(upper_prefix.begin(), upper_prefix.end(), upper_prefix.begin(), ::toupper);

                for (const auto &cmd : redis_commands)
                {
                    if (cmd.compare(0, upper_prefix.size(), upper_prefix) == 0)
                    {
                        matches.push_back(cmd);
                    }
                }

                bool already_upper = (upper_prefix == prefix);
                if (!already_upper)
                {
                    for (const auto &cmd : redis_commands)
                    {
                        std::string lower = cmd;
                        std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
                        if (lower.compare(0, prefix.size(), prefix) == 0)
                        {
                            matches.push_back(lower);
                        }
                    }
                }

                std::string lower_prefix = prefix;
                std::transform(lower_prefix.begin(), lower_prefix.end(), lower_prefix.begin(), ::tolower);
                for (const auto &cmd : bare_meta_commands)
                {
                    if (cmd.compare(0, lower_prefix.size(), lower_prefix) == 0)
                    {
                        matches.push_back(cmd);
                    }
                }
            }
        }

        if (match_index >= matches.size())
        {
            return nullptr;
        }

        return strdup(matches[match_index++].c_str());
    }

    static char **custom_completion(const char *text, int start, int end)
    {
        rl_attempted_completion_over = 1;
        return rl_completion_matches(text, completion_generator);
    }

#endif

    bool is_interactive()
    {
#if defined(HAVE_READLINE) || defined(HAVE_EDITLINE)
#ifdef _WIN32
        return _isatty(_fileno(stdin)) != 0;
#else
        return isatty(STDIN_FILENO) != 0;
#endif
#else
        return false;
#endif
    }

    void set_repl_state_for_completion(ReplState *state)
    {
        g_state_for_completion = state;
    }

    void input_init(ReplState &state)
    {
#if defined(HAVE_READLINE) || defined(HAVE_EDITLINE)
        if (is_interactive())
        {
            rl_readline_name = "yredis";
            rl_attempted_completion_function = custom_completion;

#ifdef HAVE_READLINE
            rl_completion_append_character = ' ';
            rl_basic_word_break_characters = const_cast<char *>(" \t\n\"\\'`@$><=;|&{(");
            rl_special_prefixes = const_cast<char *>("\\");
#else
            rl_basic_word_break_characters = const_cast<char *>(" \t\n\"\\'`@$><=;|&{(");
#endif

            set_repl_state_for_completion(&state);

            std::string path = get_history_path();
            read_history(path.c_str());
            stifle_history(max_history_entries);
        }
#endif
    }

    void input_cleanup()
    {
#if defined(HAVE_READLINE) || defined(HAVE_EDITLINE)
        if (is_interactive())
        {
            save_history();
        }
#endif
    }

    ReadResult read_line(const std::string &prompt)
    {
#if defined(HAVE_READLINE) || defined(HAVE_EDITLINE)
        if (is_interactive())
        {
            char *line_read = readline(prompt.c_str());
            if (!line_read)
            {
                return {{}, true};
            }

            std::string result(line_read);
            free(line_read);

            if (!result.empty())
            {
                ::add_history(result.c_str());
            }

            return {result, false};
        }
#endif

        std::cout << prompt;
        std::string line;
        if (!std::getline(std::cin, line))
        {
            return {{}, true};
        }
        return {line, false};
    }

    void add_history_entry(const std::string &line)
    {
#if defined(HAVE_READLINE) || defined(HAVE_EDITLINE)
        if (is_interactive() && !line.empty())
        {
            ::add_history(line.c_str());
        }
#endif
    }

    void load_history()
    {
#if defined(HAVE_READLINE) || defined(HAVE_EDITLINE)
        if (is_interactive())
        {
            std::string path = get_history_path();
            read_history(path.c_str());
        }
#endif
    }

    void save_history()
    {
#if defined(HAVE_READLINE) || defined(HAVE_EDITLINE)
        if (is_interactive())
        {
            std::string path = get_history_path();
            write_history(path.c_str());
#ifdef HAVE_READLINE
            history_truncate_file(path.c_str(), max_history_entries);
#else
            stifle_history(max_history_entries);
#endif
        }
#endif
    }

    void print_history(int max_lines)
    {
#if defined(HAVE_READLINE) || defined(HAVE_EDITLINE)
        if (!is_interactive())
        {
            std::cout << ansi_gray() << "(history only available in interactive mode)" << ansi_reset() << "\n";
            return;
        }

        HIST_ENTRY **list = history_list();
        if (!list)
        {
            std::cout << ansi_gray() << "(no history)" << ansi_reset() << "\n";
            return;
        }

        int total = 0;
        while (list[total]) total++;

        int start = 0;
        if (max_lines > 0 && total > max_lines)
        {
            start = total - max_lines;
        }

        for (int i = start; i < total; ++i)
        {
            std::cout << ansi_gray() << std::setw(5) << (i + 1) << "  " << list[i]->line << ansi_reset() << "\n";
        }
#else
        std::cout << ansi_gray() << "(history not available - no readline/editline)" << ansi_reset() << "\n";
#endif
    }
}