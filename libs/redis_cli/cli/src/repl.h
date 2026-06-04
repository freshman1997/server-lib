#ifndef __YREDIS_REPL_H__
#define __YREDIS_REPL_H__

#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include "option.h"
#include "redis_client.h"
#include "formatter.h"

namespace yredis
{
    struct ReplState
    {
        std::shared_ptr<yuan::redis::RedisClient> client;
        yuan::redis::Option opt;
        FormatStyle format_style = FormatStyle::pretty;
        bool pipeline_mode = false;
        std::vector<yuan::redis::PipelineCommand> pipeline_cmds;
        bool multi_mode = false;
        bool subscribe_mode = false;
        bool connected = false;
        bool should_exit = false;
    };

    void repl_loop(ReplState &state);

    std::string build_prompt(const ReplState &state);

    void print_banner();

    void print_help();

    void handle_meta_command(ReplState &state, const std::string &cmd);

    std::vector<std::string> tokenize(const std::string &line);

    std::string to_lower(std::string s);

    bool is_meta_command(const std::string &first_token);

    std::shared_ptr<yuan::redis::RedisValue> execute_redis_command(
        ReplState &state,
        const std::string &name,
        const std::vector<std::string> &args);
}

#endif
