#include "connection/ssh_pty_process.h"
#include "platform/native_platform.h"

#include <cerrno>
#include <clocale>
#include <cstring>
#include <chrono>
#include <algorithm>
#include <string_view>

#if !defined(_WIN32)
#include <fcntl.h>
#include <cstdlib>
#include <poll.h>
#include <pwd.h>
#include <pty.h>
#include <signal.h>
#include <strings.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#endif

namespace yuan::net::ssh
{
    namespace
    {
#if !defined(_WIN32)
        enum class TerminalOpcode : uint8_t {
            kVINTR = 1,
            kVQUIT = 2,
            kVERASE = 3,
            kVKILL = 4,
            kVEOF = 5,
            kVEOL = 6,
            kVEOL2 = 7,
            kVSTART = 8,
            kVSTOP = 9,
            kVSUSP = 10,
            kVDSUSP = 11,
            kVREPRINT = 12,
            kVWERASE = 13,
            kVLNEXT = 14,
            kVFLUSH = 15,
            kVSWTCH = 16,
            kVSTATUS = 17,
            kVDISCARD = 18,
            kIGNPAR = 30,
            kPARMRK = 31,
            kINPCK = 32,
            kISTRIP = 33,
            kINLCR = 34,
            kIGNCR = 35,
            kICRNL = 36,
            kIUCLC = 37,
            kIXON = 38,
            kIXANY = 39,
            kIXOFF = 40,
            kIMAXBEL = 41,
            kISIG = 50,
            kICANON = 51,
            kXCASE = 52,
            kECHO = 53,
            kECHOE = 54,
            kECHOK = 55,
            kECHONL = 56,
            kNOFLSH = 57,
            kTOSTOP = 58,
            kIEXTEN = 59,
            kECHOCTL = 60,
            kECHOKE = 61,
            kPENDIN = 62,
            kOPOST = 70,
            kOLCUC = 71,
            kONLCR = 72,
            kOCRNL = 73,
            kONOCR = 74,
            kONLRET = 75,
            kCS7 = 90,
            kCS8 = 91,
            kPARENB = 92,
            kPARODD = 93,
            kTTY_OP_ISPEED = 128,
            kTTY_OP_OSPEED = 129
        };

        void apply_termios_flag(tcflag_t &field, tcflag_t flag, uint32_t value)
        {
            if (value != 0) {
                field |= flag;
            } else {
                field &= ~flag;
            }
        }

        bool read_terminal_mode_u32(const std::vector<uint8_t> &modes,
                                    size_t &offset,
                                    uint32_t &value)
        {
            if (offset + 4 > modes.size()) {
                return false;
            }
            value = (static_cast<uint32_t>(modes[offset]) << 24) |
                    (static_cast<uint32_t>(modes[offset + 1]) << 16) |
                    (static_cast<uint32_t>(modes[offset + 2]) << 8) |
                    static_cast<uint32_t>(modes[offset + 3]);
            offset += 4;
            return true;
        }

        void apply_control_char(struct termios &term, size_t index, uint32_t value)
        {
            if (index >= NCCS) {
                return;
            }
            term.c_cc[index] = value == 0xFFFFFFFFu ? _POSIX_VDISABLE : static_cast<cc_t>(value & 0xFFu);
        }

        speed_t terminal_speed_from_baud(uint32_t baud)
        {
            switch (baud) {
#ifdef B0
            case 0: return B0;
#endif
#ifdef B50
            case 50: return B50;
#endif
#ifdef B75
            case 75: return B75;
#endif
#ifdef B110
            case 110: return B110;
#endif
#ifdef B134
            case 134: return B134;
#endif
#ifdef B150
            case 150: return B150;
#endif
#ifdef B200
            case 200: return B200;
#endif
#ifdef B300
            case 300: return B300;
#endif
#ifdef B600
            case 600: return B600;
#endif
#ifdef B1200
            case 1200: return B1200;
#endif
#ifdef B1800
            case 1800: return B1800;
#endif
#ifdef B2400
            case 2400: return B2400;
#endif
#ifdef B4800
            case 4800: return B4800;
#endif
#ifdef B9600
            case 9600: return B9600;
#endif
#ifdef B19200
            case 19200: return B19200;
#endif
#ifdef B38400
            case 38400: return B38400;
#endif
#ifdef B57600
            case 57600: return B57600;
#endif
#ifdef B115200
            case 115200: return B115200;
#endif
#ifdef B230400
            case 230400: return B230400;
#endif
            default: return static_cast<speed_t>(0);
            }
        }

        bool parse_terminal_modes(const std::vector<uint8_t> &modes, struct termios &term)
        {
            size_t offset = 0;
            while (offset < modes.size()) {
                const auto opcode = static_cast<TerminalOpcode>(modes[offset++]);
                if (static_cast<uint8_t>(opcode) == 0) {
                    return true;
                }

                uint32_t value = 0;
                if (!read_terminal_mode_u32(modes, offset, value)) {
                    return false;
                }

                switch (opcode) {
                case TerminalOpcode::kVINTR:
                    apply_control_char(term, VINTR, value);
                    break;
                case TerminalOpcode::kVQUIT:
                    apply_control_char(term, VQUIT, value);
                    break;
                case TerminalOpcode::kVERASE:
                    apply_control_char(term, VERASE, value);
                    break;
                case TerminalOpcode::kVKILL:
                    apply_control_char(term, VKILL, value);
                    break;
                case TerminalOpcode::kVEOF:
                    apply_control_char(term, VEOF, value);
                    break;
                case TerminalOpcode::kVEOL:
                    apply_control_char(term, VEOL, value);
                    break;
#ifdef VEOL2
                case TerminalOpcode::kVEOL2:
                    apply_control_char(term, VEOL2, value);
                    break;
#endif
                case TerminalOpcode::kVSTART:
                    apply_control_char(term, VSTART, value);
                    break;
                case TerminalOpcode::kVSTOP:
                    apply_control_char(term, VSTOP, value);
                    break;
                case TerminalOpcode::kVSUSP:
                    apply_control_char(term, VSUSP, value);
                    break;
#ifdef VREPRINT
                case TerminalOpcode::kVREPRINT:
                    apply_control_char(term, VREPRINT, value);
                    break;
#endif
#ifdef VWERASE
                case TerminalOpcode::kVWERASE:
                    apply_control_char(term, VWERASE, value);
                    break;
#endif
#ifdef VLNEXT
                case TerminalOpcode::kVLNEXT:
                    apply_control_char(term, VLNEXT, value);
                    break;
#endif
#ifdef VDISCARD
                case TerminalOpcode::kVDISCARD:
                    apply_control_char(term, VDISCARD, value);
                    break;
#endif
                case TerminalOpcode::kIGNPAR:
                    apply_termios_flag(term.c_iflag, IGNPAR, value);
                    break;
                case TerminalOpcode::kPARMRK:
                    apply_termios_flag(term.c_iflag, PARMRK, value);
                    break;
                case TerminalOpcode::kINPCK:
                    apply_termios_flag(term.c_iflag, INPCK, value);
                    break;
                case TerminalOpcode::kISTRIP:
                    apply_termios_flag(term.c_iflag, ISTRIP, value);
                    break;
                case TerminalOpcode::kINLCR:
                    apply_termios_flag(term.c_iflag, INLCR, value);
                    break;
                case TerminalOpcode::kIGNCR:
                    apply_termios_flag(term.c_iflag, IGNCR, value);
                    break;
                case TerminalOpcode::kICRNL:
                    apply_termios_flag(term.c_iflag, ICRNL, value);
                    break;
                case TerminalOpcode::kIXON:
                    apply_termios_flag(term.c_iflag, IXON, value);
                    break;
#ifdef IXANY
                case TerminalOpcode::kIXANY:
                    apply_termios_flag(term.c_iflag, IXANY, value);
                    break;
#endif
#ifdef IXOFF
                case TerminalOpcode::kIXOFF:
                    apply_termios_flag(term.c_iflag, IXOFF, value);
                    break;
#endif
                case TerminalOpcode::kISIG:
                    apply_termios_flag(term.c_lflag, ISIG, value);
                    break;
                case TerminalOpcode::kICANON:
                    apply_termios_flag(term.c_lflag, ICANON, value);
                    break;
                case TerminalOpcode::kECHO:
                    apply_termios_flag(term.c_lflag, ECHO, value);
                    break;
                case TerminalOpcode::kECHOE:
                    apply_termios_flag(term.c_lflag, ECHOE, value);
                    break;
                case TerminalOpcode::kECHOK:
                    apply_termios_flag(term.c_lflag, ECHOK, value);
                    break;
                case TerminalOpcode::kECHONL:
                    apply_termios_flag(term.c_lflag, ECHONL, value);
                    break;
                case TerminalOpcode::kNOFLSH:
                    apply_termios_flag(term.c_lflag, NOFLSH, value);
                    break;
                case TerminalOpcode::kTOSTOP:
                    apply_termios_flag(term.c_lflag, TOSTOP, value);
                    break;
#ifdef IEXTEN
                case TerminalOpcode::kIEXTEN:
                    apply_termios_flag(term.c_lflag, IEXTEN, value);
                    break;
#endif
#ifdef ECHOCTL
                case TerminalOpcode::kECHOCTL:
                    apply_termios_flag(term.c_lflag, ECHOCTL, value);
                    break;
#endif
#ifdef ECHOKE
                case TerminalOpcode::kECHOKE:
                    apply_termios_flag(term.c_lflag, ECHOKE, value);
                    break;
#endif
                case TerminalOpcode::kOPOST:
                    apply_termios_flag(term.c_oflag, OPOST, value);
                    break;
#ifdef ONLCR
                case TerminalOpcode::kONLCR:
                    apply_termios_flag(term.c_oflag, ONLCR, value);
                    break;
#endif
#ifdef OCRNL
                case TerminalOpcode::kOCRNL:
                    apply_termios_flag(term.c_oflag, OCRNL, value);
                    break;
#endif
#ifdef ONOCR
                case TerminalOpcode::kONOCR:
                    apply_termios_flag(term.c_oflag, ONOCR, value);
                    break;
#endif
#ifdef ONLRET
                case TerminalOpcode::kONLRET:
                    apply_termios_flag(term.c_oflag, ONLRET, value);
                    break;
#endif
                case TerminalOpcode::kCS7:
                    if (value != 0) {
                        term.c_cflag = (term.c_cflag & ~CSIZE) | CS7;
                    }
                    break;
                case TerminalOpcode::kCS8:
                    if (value != 0) {
                        term.c_cflag = (term.c_cflag & ~CSIZE) | CS8;
                    }
                    break;
                case TerminalOpcode::kPARENB:
                    apply_termios_flag(term.c_cflag, PARENB, value);
                    break;
                case TerminalOpcode::kPARODD:
                    apply_termios_flag(term.c_cflag, PARODD, value);
                    break;
                case TerminalOpcode::kTTY_OP_ISPEED: {
                    const speed_t speed = terminal_speed_from_baud(value);
                    if (speed != static_cast<speed_t>(0) || value == 0) {
                        cfsetispeed(&term, speed);
                    }
                    break;
                }
                case TerminalOpcode::kTTY_OP_OSPEED: {
                    const speed_t speed = terminal_speed_from_baud(value);
                    if (speed != static_cast<speed_t>(0) || value == 0) {
                        cfsetospeed(&term, speed);
                    }
                    break;
                }
                default:
                    break;
                }
            }
            return true;
        }

        void apply_sane_terminal_defaults(int fd)
        {
            struct termios term;
            if (tcgetattr(fd, &term) != 0) {
                return;
            }

            term.c_iflag |= (ICRNL | IXON);
            term.c_iflag &= ~(INLCR | IGNCR);

            term.c_oflag |= OPOST;
#ifdef ONLCR
            term.c_oflag |= ONLCR;
#endif

            term.c_lflag |= (ECHO | ECHOE | ECHOK | ICANON | ISIG | IEXTEN);

            term.c_cc[VERASE] = 127;
            term.c_cc[VINTR] = 3;
            term.c_cc[VEOF] = 4;

            tcsetattr(fd, TCSANOW, &term);
        }

        void apply_requested_terminal_modes(int fd, const std::vector<uint8_t> &modes)
        {
            if (modes.empty()) {
                return;
            }

            struct termios term;
            if (tcgetattr(fd, &term) != 0) {
                return;
            }

            if (parse_terminal_modes(modes, term)) {
                tcsetattr(fd, TCSANOW, &term);
            }
        }

        void apply_environment(const SshTerminalSpec &spec)
        {
            auto has_utf8_locale = [](const char *value)->bool {
                if (!value || *value == '\0') {
                    return false;
                }
                std::string_view v(value);
                return v.find("UTF-8") != std::string_view::npos ||
                       v.find("utf8") != std::string_view::npos ||
                       v.find("utf-8") != std::string_view::npos;
            };

            auto is_c_utf8 = [](const char *value)->bool {
                if (!value || *value == '\0') {
                    return false;
                }
                std::string_view v(value);
                return v == "C.UTF-8" || v == "C.utf8" || v == "c.utf8";
            };

            auto apply_locale_candidate = [](const char *candidate)->bool {
                if (!candidate || *candidate == '\0') {
                    return false;
                }
                if (!setlocale(LC_CTYPE, candidate)) {
                    return false;
                }
                setenv("LANG", candidate, 1);
                setenv("LC_CTYPE", candidate, 1);
                return true;
            };

            if (!spec.term_env.empty()) {
                setenv("TERM", spec.term_env.c_str(), 1);
            } else {
                setenv("TERM", "xterm-256color", 1);
            }

            if (const passwd *pw = getpwuid(getuid())) {
                if (pw->pw_name && *pw->pw_name != '\0') {
                    setenv("USER", pw->pw_name, 1);
                    setenv("LOGNAME", pw->pw_name, 1);
                }
                if (pw->pw_dir && *pw->pw_dir != '\0') {
                    setenv("HOME", pw->pw_dir, 1);
                    const int ignored_chdir_result = chdir(pw->pw_dir);
                    (void)ignored_chdir_result;
                }
            }

            setenv("PATH", "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin", 1);

            const char *env_lc_all = std::getenv("LC_ALL");
            const char *env_lang = std::getenv("LANG");
            const char *env_lc_ctype = std::getenv("LC_CTYPE");

            unsetenv("LC_ALL");

            bool locale_set = false;

            if (has_utf8_locale(env_lang) && !is_c_utf8(env_lang)) {
                locale_set = apply_locale_candidate(env_lang);
            }
            if (!locale_set && has_utf8_locale(env_lc_ctype) && !is_c_utf8(env_lc_ctype)) {
                locale_set = apply_locale_candidate(env_lc_ctype);
            }
            if (!locale_set && has_utf8_locale(env_lc_all) && !is_c_utf8(env_lc_all)) {
                locale_set = apply_locale_candidate(env_lc_all);
            }
            if (!locale_set) {
                locale_set = apply_locale_candidate("en_US.UTF-8");
            }
            if (!locale_set) {
                locale_set = apply_locale_candidate("zh_CN.UTF-8");
            }
            if (!locale_set) {
                locale_set = apply_locale_candidate("C.UTF-8");
            }

            if (!locale_set) {
                setlocale(LC_CTYPE, "C");
                unsetenv("LANG");
                unsetenv("LC_CTYPE");
            }
        }

        std::string preferred_shell_path()
        {
            if (const passwd *pw = getpwuid(getuid())) {
                if (pw->pw_shell && *pw->pw_shell != '\0' && access(pw->pw_shell, X_OK) == 0) {
                    return std::string(pw->pw_shell);
                }
            }

            const char *env_shell = std::getenv("SHELL");
            if (env_shell && *env_shell != '\0' && access(env_shell, X_OK) == 0) {
                return std::string(env_shell);
            }

            if (access("/bin/bash", X_OK) == 0) {
                return "/bin/bash";
            }

            return "/bin/sh";
        }

        std::string shell_argv0(const std::string &shell_path)
        {
            const size_t pos = shell_path.find_last_of('/');
            if (pos == std::string::npos || pos + 1 >= shell_path.size()) {
                return shell_path;
            }
            return shell_path.substr(pos + 1);
        }
#endif

        void maybe_set_error(std::string *error_message, const std::string &message)
        {
            if (error_message) {
                *error_message = message;
            }
        }
    }

    SshPtyBackend::~SshPtyBackend()
    {
        release();
    }

    bool SshPtyBackend::allocate(const SshTerminalSpec & spec, std::string * error_message)
    {
        release();

#if defined(_WIN32)
        maybe_set_error(error_message, "PTY is not implemented on Windows yet");
        return false;
#else
        int master = -1;
        int slave = -1;
        if (::openpty(&master, &slave, nullptr, nullptr, nullptr) != 0) {
            const int err = yuan::platform::GetLastSystemError();
            maybe_set_error(error_message, std::string("openpty failed: ") + yuan::platform::DescribeNativeError(err));
            return false;
        }

        struct winsize ws;
        std::memset(&ws, 0, sizeof(ws));
        ws.ws_col = static_cast<unsigned short>(spec.width > 0 ? spec.width : 80);
        ws.ws_row = static_cast<unsigned short>(spec.height > 0 ? spec.height : 24);
        ws.ws_xpixel = static_cast<unsigned short>(spec.pixel_width);
        ws.ws_ypixel = static_cast<unsigned short>(spec.pixel_height);
        ioctl(slave, TIOCSWINSZ, &ws);

        const int flags = fcntl(master, F_GETFL, 0);
        if (flags >= 0) {
            fcntl(master, F_SETFL, flags | O_NONBLOCK);
        }

        master_fd_ = master;
        slave_fd_ = slave;
        return true;
#endif
    }

    void SshPtyBackend::release()
    {
#if !defined(_WIN32)
        if (master_fd_ >= 0) {
            close(master_fd_);
            master_fd_ = -1;
        }
        if (slave_fd_ >= 0) {
            close(slave_fd_);
            slave_fd_ = -1;
        }
#else
        master_fd_ = -1;
        slave_fd_ = -1;
#endif
    }

    bool SshPtyBackend::allocated() const
    {
        return master_fd_ >= 0 && slave_fd_ >= 0;
    }

    int SshPtyBackend::master_fd() const
    {
        return master_fd_;
    }

    int SshPtyBackend::slave_fd() const
    {
        return slave_fd_;
    }

    SshPtyProcess::~SshPtyProcess()
    {
        shutdown();
    }

    bool SshPtyProcess::prepare(const SshTerminalSpec & spec, std::string * error_message)
    {
        shutdown();
        if (!backend_.allocate(spec, error_message)) {
            return false;
        }

#if !defined(_WIN32)
        if (backend_.slave_fd() >= 0) {
            apply_sane_terminal_defaults(backend_.slave_fd());
            apply_requested_terminal_modes(backend_.slave_fd(), spec.terminal_modes);
        }
#endif

        spec_ = spec;
        return true;
    }

    bool SshPtyProcess::launch_shell(const std::string & command,
                                     bool interactive,
                                     std::string * error_message)
    {
#if defined(_WIN32)
        (void)command;
        (void)interactive;
        maybe_set_error(error_message, "PTY shell launch is not implemented on Windows yet");
        return false;
#else
        if (!ready()) {
            maybe_set_error(error_message, "PTY backend is not prepared");
            return false;
        }
        if (child_pid_ > 0) {
            maybe_set_error(error_message, "PTY child is already running");
            return false;
        }

        const int pid = fork();
        if (pid < 0) {
            const int err = yuan::platform::GetLastSystemError();
            maybe_set_error(error_message, std::string("fork failed: ") + yuan::platform::DescribeNativeError(err));
            return false;
        }

        if (pid == 0) {
            setsid();
            ioctl(backend_.slave_fd(), TIOCSCTTY, 0);
            setpgid(0, 0);
            dup2(backend_.slave_fd(), STDIN_FILENO);
            dup2(backend_.slave_fd(), STDOUT_FILENO);
            dup2(backend_.slave_fd(), STDERR_FILENO);
            tcsetpgrp(STDIN_FILENO, getpgrp());
            if (backend_.master_fd() >= 0) {
                close(backend_.master_fd());
            }
            if (backend_.slave_fd() > STDERR_FILENO) {
                close(backend_.slave_fd());
            }

            apply_environment(spec_);

            const std::string shell_path = preferred_shell_path();
            const std::string argv0 = shell_argv0(shell_path);

            const bool use_bash = shell_path.find("bash") != std::string::npos;
            if (interactive) {
                if (command.empty()) {
                    if (use_bash) {
                        execl(shell_path.c_str(), "bash", "-i", static_cast<char *>(nullptr));
                    } else {
                        execl(shell_path.c_str(), argv0.c_str(), "-i", static_cast<char *>(nullptr));
                    }
                } else {
                    if (use_bash) {
                        execl(shell_path.c_str(), "bash", "-ic", command.c_str(), static_cast<char *>(nullptr));
                    } else {
                        execl(shell_path.c_str(), argv0.c_str(), "-ic", command.c_str(), static_cast<char *>(nullptr));
                    }
                }
            } else {
                if (command.empty()) {
                    if (use_bash) {
                        execl(shell_path.c_str(), "bash", static_cast<char *>(nullptr));
                    } else {
                        execl(shell_path.c_str(), argv0.c_str(), static_cast<char *>(nullptr));
                    }
                } else {
                    if (use_bash) {
                        execl(shell_path.c_str(), "bash", "-lc", command.c_str(), static_cast<char *>(nullptr));
                    } else {
                        execl(shell_path.c_str(), argv0.c_str(), "-lc", command.c_str(), static_cast<char *>(nullptr));
                    }
                }
            }
            _exit(127);
        }

        child_pid_ = pid;
        return true;
#endif
    }

    bool SshPtyProcess::write_input(const uint8_t * data, size_t len, size_t * written)
    {
        if (written) {
            *written = 0;
        }
#if defined(_WIN32)
        (void)data;
        (void)len;
        return false;
#else
        if (!ready() || !data || len == 0) {
            return false;
        }
        size_t total_written = 0;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(2000);
        while (total_written < len) {
            const ssize_t n = write(backend_.master_fd(), data + total_written, len - total_written);
            if (n > 0) {
                total_written += static_cast<size_t>(n);
                continue;
            }

            if (n == 0) {
                break;
            }

            const int write_error = yuan::platform::GetLastSystemError();
            if (yuan::platform::ClassifyNativeError(write_error) == yuan::platform::NativeError::interrupted) {
                continue;
            }

            if (yuan::platform::ClassifyNativeError(write_error) == yuan::platform::NativeError::would_block) {
                if (std::chrono::steady_clock::now() >= deadline) {
                    break;
                }

                pollfd pfd{};
                pfd.fd = backend_.master_fd();
                pfd.events = POLLOUT;
                const int rc = poll(&pfd, 1, 5);
                if (rc < 0 && yuan::platform::ClassifyNativeError(yuan::platform::GetLastSystemError()) != yuan::platform::NativeError::interrupted) {
                    return false;
                }
                continue;
            }

            return false;
        }

        if (written) {
            *written = total_written;
        }
        return total_written > 0;
#endif
    }

    bool SshPtyProcess::read_output(std::vector<uint8_t> * out, size_t max_bytes)
    {
        if (!out) {
            return false;
        }
        out->clear();
#if defined(_WIN32)
        (void)max_bytes;
        return false;
#else
        if (!ready()) {
            return false;
        }

        const size_t cap = max_bytes > 0 ? max_bytes : 1;
        out->reserve(cap);

        std::vector<uint8_t> chunk(4096);
        while (out->size() < cap) {
            const size_t remaining = cap - out->size();
            const size_t read_size = std::min(remaining, chunk.size());
            const ssize_t n = read(backend_.master_fd(), chunk.data(), read_size);
            if (n > 0) {
                out->insert(out->end(), chunk.begin(), chunk.begin() + n);
                if (static_cast<size_t>(n) < read_size) {
                    break;
                }
                continue;
            }

            if (n == 0) {
                break;
            }

            if (yuan::platform::IsNativeRetryableError(yuan::platform::GetLastSystemError())) {
                break;
            }
            return false;
        }

        return !out->empty();
#endif
    }

    bool SshPtyProcess::resize_terminal(uint32_t width,
                                        uint32_t height,
                                        uint32_t pixel_width,
                                        uint32_t pixel_height,
                                        std::string * error_message)
    {
#if defined(_WIN32)
        (void)width;
        (void)height;
        (void)pixel_width;
        (void)pixel_height;
        maybe_set_error(error_message, "PTY resize is not implemented on Windows yet");
        return false;
#else
        if (!ready()) {
            maybe_set_error(error_message, "PTY backend is not prepared");
            return false;
        }

        struct winsize ws;
        std::memset(&ws, 0, sizeof(ws));
        ws.ws_col = static_cast<unsigned short>(width > 0 ? width : 80);
        ws.ws_row = static_cast<unsigned short>(height > 0 ? height : 24);
        ws.ws_xpixel = static_cast<unsigned short>(pixel_width);
        ws.ws_ypixel = static_cast<unsigned short>(pixel_height);

        if (ioctl(backend_.master_fd(), TIOCSWINSZ, &ws) != 0) {
            const int err = yuan::platform::GetLastSystemError();
            maybe_set_error(error_message, std::string("TIOCSWINSZ failed: ") + yuan::platform::DescribeNativeError(err));
            return false;
        }
        return true;
#endif
    }

    bool SshPtyProcess::send_signal(const std::string & signal_name,
                                    std::string * error_message)
    {
#if defined(_WIN32)
        (void)signal_name;
        maybe_set_error(error_message, "PTY signal forwarding is not implemented on Windows yet");
        return false;
#else
        if (child_pid_ <= 0) {
            maybe_set_error(error_message, "PTY child is not running");
            return false;
        }

        int sig = 0;
        if (strcasecmp(signal_name.c_str(), "TERM") == 0 || strcasecmp(signal_name.c_str(), "SIGTERM") == 0) {
            sig = SIGTERM;
        } else if (strcasecmp(signal_name.c_str(), "INT") == 0 || strcasecmp(signal_name.c_str(), "SIGINT") == 0) {
            sig = SIGINT;
        } else if (strcasecmp(signal_name.c_str(), "KILL") == 0 || strcasecmp(signal_name.c_str(), "SIGKILL") == 0) {
            sig = SIGKILL;
        } else if (strcasecmp(signal_name.c_str(), "HUP") == 0 || strcasecmp(signal_name.c_str(), "SIGHUP") == 0) {
            sig = SIGHUP;
        } else if (strcasecmp(signal_name.c_str(), "QUIT") == 0 || strcasecmp(signal_name.c_str(), "SIGQUIT") == 0) {
            sig = SIGQUIT;
        } else {
            maybe_set_error(error_message, "unsupported signal name for PTY process");
            return false;
        }

        if (kill(-child_pid_, sig) != 0 && kill(child_pid_, sig) != 0) {
            const int err = yuan::platform::GetLastSystemError();
            maybe_set_error(error_message, std::string("kill failed: ") + yuan::platform::DescribeNativeError(err));
            return false;
        }
        return true;
#endif
    }

    bool SshPtyProcess::poll_exit(SshPtyExitState * state)
    {
        if (state) {
            *state = {};
        }
#if defined(_WIN32)
        return false;
#else
        if (child_pid_ <= 0) {
            return false;
        }

        int status = 0;
        const pid_t rc = waitpid(child_pid_, &status, WNOHANG);
        if (rc == 0 || rc < 0) {
            return false;
        }

        if (state) {
            state->exited = WIFEXITED(status);
            state->signaled = WIFSIGNALED(status);
            state->exit_code = state->exited ? WEXITSTATUS(status) : -1;
            state->term_signal = state->signaled ? WTERMSIG(status) : 0;
        }

        child_pid_ = -1;
        return true;
#endif
    }

    void SshPtyProcess::shutdown()
    {
#if !defined(_WIN32)
        if (child_pid_ > 0) {
            kill(-child_pid_, SIGTERM);
            kill(child_pid_, SIGTERM);

            int status = 0;
            bool exited = false;
            for (int i = 0; i < 20; ++i) {
                const pid_t rc = waitpid(child_pid_, &status, WNOHANG);
                if (rc == child_pid_) {
                    exited = true;
                    break;
                }
                if (rc < 0 && yuan::platform::GetLastSystemError() == ECHILD) {
                    exited = true;
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }

            if (!exited) {
                kill(-child_pid_, SIGKILL);
                kill(child_pid_, SIGKILL);
                waitpid(child_pid_, &status, 0);
            }
            child_pid_ = -1;
        }
#else
        child_pid_ = -1;
#endif
        backend_.release();
    }

    bool SshPtyProcess::ready() const
    {
        return backend_.allocated();
    }

    const SshPtyBackend &SshPtyProcess::backend() const
    {
        return backend_;
    }
}
