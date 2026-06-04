#ifndef __YREDIS_INPUT_H__
#define __YREDIS_INPUT_H__

#include <string>
#include <vector>

namespace yredis
{
    struct ReplState;

    void input_init(ReplState &state);
    void input_cleanup();

    struct ReadResult
    {
        std::string line;
        bool eof = false;
    };

    ReadResult read_line(const std::string &prompt);

    void add_history_entry(const std::string &line);
    void load_history();
    void save_history();

    void print_history(int max_lines);

    bool is_interactive();

    void set_repl_state_for_completion(ReplState *state);
}

#endif