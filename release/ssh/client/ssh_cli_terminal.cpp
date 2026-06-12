#include "ssh_cli_terminal.h"

#include "buffer/byte_buffer.h"
#include "platform/native_platform.h"

#include <array>
#include <cstdlib>
#include <memory>

#ifdef _WIN32
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <termios.h>
#include <unistd.h>
#endif

namespace yuan::release_ssh::client
{
#ifndef _WIN32
    struct LocalTerminalState
    {
        termios original{};
    };
#endif

    bool stdin_is_tty()
    {
#ifdef _WIN32
        HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
        if (h == INVALID_HANDLE_VALUE || h == nullptr) {
            return false;
        }
        DWORD mode = 0;
        return GetConsoleMode(h, &mode) != 0;
#else
        return isatty(STDIN_FILENO) != 0;
#endif
    }

    LocalTerminalRawGuard::~LocalTerminalRawGuard()
    {
        restore();
    }

    bool LocalTerminalRawGuard::enable()
    {
#ifdef _WIN32
        return true;
#else
        if (active_) {
            return true;
        }
        if (!stdin_is_tty()) {
            return true;
        }
        auto *state = new LocalTerminalState();
        if (tcgetattr(STDIN_FILENO, &state->original) != 0) {
            delete state;
            return false;
        }

        termios raw = state->original;
        raw.c_iflag &= static_cast<tcflag_t>(~(BRKINT | ICRNL | INPCK | ISTRIP | IXON));
        raw.c_oflag &= static_cast<tcflag_t>(~OPOST);
        raw.c_cflag |= CS8;
        raw.c_lflag &= static_cast<tcflag_t>(~(ECHO | ICANON | IEXTEN | ISIG));
        raw.c_cc[VMIN] = 1;
        raw.c_cc[VTIME] = 0;
        if (tcsetattr(STDIN_FILENO, TCSADRAIN, &raw) != 0) {
            delete state;
            return false;
        }
        state_ = state;
        active_ = true;
        return true;
#endif
    }

    void LocalTerminalRawGuard::restore()
    {
#ifndef _WIN32
        if (active_) {
            auto *state = static_cast<LocalTerminalState *>(state_);
            (void)tcsetattr(STDIN_FILENO, TCSADRAIN, &state->original);
            delete state;
            state_ = nullptr;
            active_ = false;
        }
#endif
    }

    StdinNonblockingGuard::StdinNonblockingGuard()
    {
#ifndef _WIN32
        original_flags_ = fcntl(STDIN_FILENO, F_GETFL, 0);
        enabled_ = original_flags_ >= 0;
        if (enabled_) {
            (void)fcntl(STDIN_FILENO, F_SETFL, original_flags_ | O_NONBLOCK);
        }
#endif
    }

    StdinNonblockingGuard::~StdinNonblockingGuard()
    {
#ifndef _WIN32
        if (enabled_) {
            (void)fcntl(STDIN_FILENO, F_SETFL, original_flags_);
        }
#endif
    }

    bool stdin_has_data_nonblocking()
    {
#ifdef _WIN32
        HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
        if (h == INVALID_HANDLE_VALUE || h == nullptr) {
            return false;
        }

        DWORD mode = 0;
        if (GetConsoleMode(h, &mode)) {
            return WaitForSingleObject(h, 0) == WAIT_OBJECT_0;
        }

        if (GetFileType(h) != FILE_TYPE_PIPE) {
            return false;
        }

        DWORD available = 0;
        if (!PeekNamedPipe(h, nullptr, 0, nullptr, &available, nullptr)) {
            return false;
        }
        return available > 0;
#else
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(STDIN_FILENO, &readfds);

        timeval tv{};
        tv.tv_sec = 0;
        tv.tv_usec = 0;

        const int rc = select(STDIN_FILENO + 1, &readfds, nullptr, nullptr, &tv);
        return rc > 0 && FD_ISSET(STDIN_FILENO, &readfds);
#endif
    }

    StdinPollResult read_stdin_chunk_nonblocking(std::string &out)
    {
        out.clear();
#ifdef _WIN32
        if (!stdin_has_data_nonblocking()) {
            return StdinPollResult::no_data;
        }

        std::array<char, 4096> buf{};
        HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
        if (h == INVALID_HANDLE_VALUE || h == nullptr) {
            return StdinPollResult::error;
        }

        DWORD mode = 0;
        if (GetConsoleMode(h, &mode)) {
            DWORD n_chars = 0;
            if (!ReadConsoleA(h, buf.data(), static_cast<DWORD>(buf.size()), &n_chars, nullptr)) {
                return StdinPollResult::error;
            }
            if (n_chars == 0) {
                return StdinPollResult::eof;
            }
            out.assign(buf.data(), static_cast<size_t>(n_chars));
            return StdinPollResult::data;
        }

        DWORD n = 0;
        if (!ReadFile(h, buf.data(), static_cast<DWORD>(buf.size()), &n, nullptr)) {
            const DWORD err = GetLastError();
            if (err == ERROR_BROKEN_PIPE) {
                return StdinPollResult::eof;
            }
            if (err == ERROR_NO_DATA) {
                return StdinPollResult::no_data;
            }
            return StdinPollResult::error;
        }
        if (n == 0) {
            return StdinPollResult::eof;
        }
        out.assign(buf.data(), static_cast<size_t>(n));
        return StdinPollResult::data;
#else
        if (!stdin_has_data_nonblocking()) {
            return StdinPollResult::no_data;
        }

        std::array<char, 4096> buf{};
        const ssize_t n = ::read(STDIN_FILENO, buf.data(), buf.size());
        if (n == 0) {
            return StdinPollResult::eof;
        }
        if (n < 0) {
            const int err = yuan::platform::GetLastNativeError();
            if (yuan::platform::IsNativeRetryableError(err)) {
                return StdinPollResult::no_data;
            }
            return StdinPollResult::error;
        }
        out.assign(buf.data(), static_cast<size_t>(n));
        return StdinPollResult::data;
#endif
    }

    TerminalSize query_terminal_size()
    {
        TerminalSize ts;
#ifndef _WIN32
        winsize ws{};
        if (ioctl(STDIN_FILENO, TIOCGWINSZ, &ws) == 0) {
            if (ws.ws_col > 0) {
                ts.cols = ws.ws_col;
            }
            if (ws.ws_row > 0) {
                ts.rows = ws.ws_row;
            }
            ts.pixel_width = ws.ws_xpixel;
            ts.pixel_height = ws.ws_ypixel;
        }
#endif
        return ts;
    }

    std::string detect_terminal_type()
    {
        const char *term = std::getenv("TERM");
        if (term && *term) {
            return term;
        }
        return "xterm-256color";
    }

    namespace
    {
        std::vector<uint8_t> buffer_to_bytes(yuan::buffer::ByteBuffer &buffer)
        {
            return {
                reinterpret_cast<const uint8_t *>(buffer.read_ptr()),
                reinterpret_cast<const uint8_t *>(buffer.read_ptr()) + buffer.readable_bytes()
            };
        }

        void write_terminal_mode_u32(yuan::buffer::ByteBuffer &data, uint8_t opcode, uint32_t value)
        {
            data.append_u8(opcode);
            data.append_u8(static_cast<uint8_t>((value >> 24) & 0xff));
            data.append_u8(static_cast<uint8_t>((value >> 16) & 0xff));
            data.append_u8(static_cast<uint8_t>((value >> 8) & 0xff));
            data.append_u8(static_cast<uint8_t>(value & 0xff));
        }

#ifndef _WIN32
        uint32_t terminal_control_char_value(const termios &term, size_t index)
        {
            if (index >= NCCS) {
                return 0xff;
            }
            return term.c_cc[index] == _POSIX_VDISABLE ? 0xff : static_cast<uint32_t>(term.c_cc[index]);
        }

        uint32_t terminal_speed_to_baud(speed_t speed)
        {
            switch (speed) {
#ifdef B0
            case B0: return 0;
#endif
#ifdef B50
            case B50: return 50;
#endif
#ifdef B75
            case B75: return 75;
#endif
#ifdef B110
            case B110: return 110;
#endif
#ifdef B134
            case B134: return 134;
#endif
#ifdef B150
            case B150: return 150;
#endif
#ifdef B200
            case B200: return 200;
#endif
#ifdef B300
            case B300: return 300;
#endif
#ifdef B600
            case B600: return 600;
#endif
#ifdef B1200
            case B1200: return 1200;
#endif
#ifdef B1800
            case B1800: return 1800;
#endif
#ifdef B2400
            case B2400: return 2400;
#endif
#ifdef B4800
            case B4800: return 4800;
#endif
#ifdef B9600
            case B9600: return 9600;
#endif
#ifdef B19200
            case B19200: return 19200;
#endif
#ifdef B38400
            case B38400: return 38400;
#endif
#ifdef B57600
            case B57600: return 57600;
#endif
#ifdef B115200
            case B115200: return 115200;
#endif
#ifdef B230400
            case B230400: return 230400;
#endif
            default: return 38400;
            }
        }
#endif
    }

    std::vector<uint8_t> make_terminal_modes()
    {
#ifdef _WIN32
        return {0};
#else
        yuan::buffer::ByteBuffer data;
        termios term{};
        if (stdin_is_tty() && tcgetattr(STDIN_FILENO, &term) == 0) {
            write_terminal_mode_u32(data, 1, terminal_control_char_value(term, VINTR));
            write_terminal_mode_u32(data, 2, terminal_control_char_value(term, VQUIT));
            write_terminal_mode_u32(data, 3, terminal_control_char_value(term, VERASE));
            write_terminal_mode_u32(data, 4, terminal_control_char_value(term, VKILL));
            write_terminal_mode_u32(data, 5, terminal_control_char_value(term, VEOF));
            write_terminal_mode_u32(data, 8, terminal_control_char_value(term, VSTART));
            write_terminal_mode_u32(data, 9, terminal_control_char_value(term, VSTOP));
            write_terminal_mode_u32(data, 10, terminal_control_char_value(term, VSUSP));
            write_terminal_mode_u32(data, 50, (term.c_lflag & ISIG) ? 1u : 0u);
            write_terminal_mode_u32(data, 51, (term.c_lflag & ICANON) ? 1u : 0u);
            write_terminal_mode_u32(data, 53, (term.c_lflag & ECHO) ? 1u : 0u);
            write_terminal_mode_u32(data, 54, (term.c_lflag & ECHOE) ? 1u : 0u);
            write_terminal_mode_u32(data, 55, (term.c_lflag & ECHOK) ? 1u : 0u);
            write_terminal_mode_u32(data, 59, (term.c_lflag & IEXTEN) ? 1u : 0u);
            write_terminal_mode_u32(data, 36, (term.c_iflag & ICRNL) ? 1u : 0u);
            write_terminal_mode_u32(data, 38, (term.c_iflag & IXON) ? 1u : 0u);
            write_terminal_mode_u32(data, 70, (term.c_oflag & OPOST) ? 1u : 0u);
#ifdef ONLCR
            write_terminal_mode_u32(data, 72, (term.c_oflag & ONLCR) ? 1u : 0u);
#endif
            write_terminal_mode_u32(data, 91, (term.c_cflag & CS8) == CS8 ? 1u : 0u);
            write_terminal_mode_u32(data, 92, (term.c_cflag & PARENB) ? 1u : 0u);
            write_terminal_mode_u32(data, 128, terminal_speed_to_baud(cfgetispeed(&term)));
            write_terminal_mode_u32(data, 129, terminal_speed_to_baud(cfgetospeed(&term)));
        }
        data.append_u8(0);
        return buffer_to_bytes(data);
#endif
    }
}
