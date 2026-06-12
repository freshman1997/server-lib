#ifndef YUAN_RELEASE_SSH_CLI_TERMINAL_H
#define YUAN_RELEASE_SSH_CLI_TERMINAL_H

#include <cstdint>
#include <string>
#include <vector>

namespace yuan::release_ssh::client
{
    enum class StdinPollResult {
        no_data,
        data,
        eof,
        error
    };

    struct TerminalSize
    {
        uint32_t cols = 120;
        uint32_t rows = 40;
        uint32_t pixel_width = 0;
        uint32_t pixel_height = 0;
    };

    class LocalTerminalRawGuard
    {
    public:
        LocalTerminalRawGuard() = default;
        LocalTerminalRawGuard(const LocalTerminalRawGuard &) = delete;
        LocalTerminalRawGuard &operator=(const LocalTerminalRawGuard &) = delete;
        ~LocalTerminalRawGuard();

        bool enable();

    private:
        void restore();

#ifndef _WIN32
        void *state_ = nullptr;
#endif
        bool active_ = false;
    };

    class StdinNonblockingGuard
    {
    public:
        StdinNonblockingGuard();
        StdinNonblockingGuard(const StdinNonblockingGuard &) = delete;
        StdinNonblockingGuard &operator=(const StdinNonblockingGuard &) = delete;
        ~StdinNonblockingGuard();

    private:
#ifndef _WIN32
        bool enabled_ = false;
        int original_flags_ = 0;
#endif
    };

    bool stdin_is_tty();
    bool stdin_has_data_nonblocking();
    StdinPollResult read_stdin_chunk_nonblocking(std::string &out);
    TerminalSize query_terminal_size();
    std::string detect_terminal_type();
    std::vector<uint8_t> make_terminal_modes();
}

#endif
