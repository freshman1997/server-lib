#include "ssh_cli_transport_process.h"

#include <array>
#include <cstdio>
#include <cstring>

#if !defined(_WIN32)
#include <fcntl.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <errno.h>
#endif

namespace yuan::libs::ssh_cli
{
    namespace
    {
        void set_error(std::string *error,
                       const std::string &message)
        {
            if (error) {
                *error = message;
            }
        }

        std::string shell_quote_single(const std::string & value)
        {
            std::string out = "'";
            for (char c : value) {
                if (c == '\'') {
                    out += "'\\''";
                } else {
                    out.push_back(c);
                }
            }
            out += "'";
            return out;
        }

        std::string build_common_ssh_flags(const SshCliConnectionOptions & options,
                                           bool force_tty)
        {
            std::string cmd = force_tty ? "ssh -tt" : "ssh -T";

            if (options.strict_host_key_checking) {
                cmd += " -oStrictHostKeyChecking=yes";
                if (!options.known_hosts_path.empty()) {
                    cmd += " -oUserKnownHostsFile=" + shell_quote_single(options.known_hosts_path);
                }
            } else {
                cmd += " -oStrictHostKeyChecking=no";
                if (!options.known_hosts_path.empty()) {
                    cmd += " -oUserKnownHostsFile=" + shell_quote_single(options.known_hosts_path);
                } else {
                    cmd += " -oUserKnownHostsFile=/dev/null";
                }
            }

            cmd += options.batch_mode ? " -oBatchMode=yes" : " -oBatchMode=no";

            for (const auto &opt : options.extra_ssh_options) {
                if (!opt.empty()) {
                    cmd += " -o" + opt;
                }
            }

            if (!options.private_key_path.empty()) {
                cmd += " -i " + shell_quote_single(options.private_key_path);
            }

            cmd += " -p " + std::to_string(options.port);
            cmd += " " + options.username + "@" + options.host;
            return cmd;
        }

        bool run_shell_command_capture(const std::string & full_cmd,
                                       std::string * output,
                                       std::string * error)
        {
#if defined(_WIN32)
            (void)full_cmd;
            (void)output;
            set_error(error, "process transport is not implemented on Windows yet");
            return false;
#else
            std::array<char, 4096> buffer{};
            std::string out;
            FILE *pipe = popen(full_cmd.c_str(), "r");
            if (!pipe) {
                set_error(error, "failed to execute ssh process");
                return false;
            }
            while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) {
                out.append(buffer.data());
            }
            const int rc = pclose(pipe);
            if (rc != 0) {
                set_error(error, "ssh process returned non-zero status");
                return false;
            }

            if (output) {
                *output = std::move(out);
            }
            return true;
#endif
        }

#if !defined(_WIN32)
        int timeout_or_default(const SshCliConnectionOptions & options)
        {
            return options.interactive_read_timeout_ms > 0
                       ? options.interactive_read_timeout_ms
                       : 80;
        }

        int poll_interval_or_default(const SshCliConnectionOptions & options)
        {
            return options.interactive_poll_interval_ms > 0
                       ? options.interactive_poll_interval_ms
                       : 10;
        }
#endif

#if !defined(_WIN32)
        bool set_nonblocking_fd(int fd,
                                std::string *error)
        {
            const int flags = fcntl(fd, F_GETFL, 0);
            if (flags < 0) {
                set_error(error, std::string("fcntl(F_GETFL) failed: ") + std::strerror(errno));
                return false;
            }
            if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) {
                set_error(error, std::string("fcntl(F_SETFL) failed: ") + std::strerror(errno));
                return false;
            }
            return true;
        }
#endif
    }

    SshCliTransportProcess::~SshCliTransportProcess()
    {
        close();
    }

    bool SshCliTransportProcess::connect(const SshCliConnectionOptions & options,
                                         std::string * error)
    {
        connected_ = false;
        authenticated_ = false;
        shell_open_ = false;
        options_ = options;

        if (options_.host.empty() || options_.port == 0 || options_.username.empty()) {
            set_error(error, "host/port/username are required");
            return false;
        }
        connected_ = true;
        return true;
    }

    bool SshCliTransportProcess::authenticate_password(const std::string & password,
                                                       std::string * error)
    {
        if (!connected_) {
            set_error(error, "not connected");
            return false;
        }
        if (password.empty()) {
            set_error(error, "password cannot be empty");
            return false;
        }
        authenticated_ = true;
        return true;
    }

    bool SshCliTransportProcess::authenticate_publickey(const std::string & private_key_path,
                                                        std::string * error)
    {
        if (!connected_) {
            set_error(error, "not connected");
            return false;
        }
        if (private_key_path.empty()) {
            set_error(error, "private key path cannot be empty");
            return false;
        }
        options_.private_key_path = private_key_path;
        authenticated_ = true;
        return true;
    }

    bool SshCliTransportProcess::open_shell(std::string * error)
    {
        if (!connected_ || !authenticated_) {
            set_error(error, "must connect and authenticate first");
            return false;
        }

#if defined(_WIN32)
        set_error(error, "interactive process transport is not implemented on Windows yet");
        return false;
#else
        if (shell_child_pid_ > 0) {
            shell_open_ = true;
            return true;
        }

        int to_child[2] = { -1, -1 };
        int from_child[2] = { -1, -1 };
        if (pipe(to_child) != 0 || pipe(from_child) != 0) {
            if (to_child[0] >= 0) {
                ::close(to_child[0]);
                ::close(to_child[1]);
            }
            if (from_child[0] >= 0) {
                ::close(from_child[0]);
                ::close(from_child[1]);
            }
            set_error(error, "failed to create process pipes");
            return false;
        }

        const pid_t pid = fork();
        if (pid < 0) {
            ::close(to_child[0]);
            ::close(to_child[1]);
            ::close(from_child[0]);
            ::close(from_child[1]);
            set_error(error, "fork failed");
            return false;
        }

        if (pid == 0) {
            dup2(to_child[0], STDIN_FILENO);
            dup2(from_child[1], STDOUT_FILENO);
            dup2(from_child[1], STDERR_FILENO);
            ::close(to_child[0]);
            ::close(to_child[1]);
            ::close(from_child[0]);
            ::close(from_child[1]);

            std::string cmd = build_common_ssh_flags(options_, true);
            cmd += " 2>/dev/null";
            execl("/bin/sh", "sh", "-c", cmd.c_str(), static_cast<char *>(nullptr));
            _exit(127);
        }

        ::close(to_child[0]);
        ::close(from_child[1]);
        shell_stdin_fd_ = to_child[1];
        shell_stdout_fd_ = from_child[0];
        shell_child_pid_ = static_cast<int>(pid);

        if (!set_nonblocking_fd(shell_stdout_fd_, error)) {
            close();
            return false;
        }

        shell_open_ = true;
        return true;
#endif
    }

    bool SshCliTransportProcess::run_command(const std::string & command,
                                             std::string * stdout_data,
                                             std::string * error)
    {
        if (!connected_ || !authenticated_) {
            set_error(error, "must connect and authenticate first");
            return false;
        }
        if (command.empty()) {
            set_error(error, "command cannot be empty");
            return false;
        }

        std::string cmd = build_common_ssh_flags(options_, false);
        cmd += " " + shell_quote_single(command);
        cmd += " 2>/dev/null";

        return run_shell_command_capture(cmd, stdout_data, error);
    }

    bool SshCliTransportProcess::send_stdin(const std::string & chunk,
                                            std::string * error)
    {
        if (!shell_open_) {
            set_error(error, "shell is not open");
            return false;
        }
        if (chunk.empty()) {
            set_error(error, "stdin chunk cannot be empty");
            return false;
        }

#if defined(_WIN32)
        set_error(error, "interactive process transport is not implemented on Windows yet");
        return false;
#else
        if (shell_stdin_fd_ < 0 || shell_stdout_fd_ < 0 || shell_child_pid_ <= 0) {
            set_error(error, "interactive shell process is not active");
            return false;
        }

        const ssize_t written = write(shell_stdin_fd_, chunk.data(), chunk.size());
        if (written < 0) {
            set_error(error, "write to interactive shell failed");
            return false;
        }
        return true;
#endif
    }

    bool SshCliTransportProcess::read_stdout_chunk(std::string * chunk,
                                                   std::string * error)
    {
#if defined(_WIN32)
        (void)chunk;
        set_error(error, "interactive process transport is not implemented on Windows yet");
        return false;
#else
        if (shell_stdout_fd_ < 0) {
            set_error(error, "interactive shell stdout is not active");
            return false;
        }
        if (!chunk) {
            set_error(error, "chunk output pointer is null");
            return false;
        }

        const int timeout_ms = timeout_or_default(options_);
        const int poll_ms = poll_interval_or_default(options_);
        auto waited = 0;
        while (waited <= timeout_ms) {
            char buf[4096];
            const ssize_t n = read(shell_stdout_fd_, buf, sizeof(buf));
            if (n > 0) {
                chunk->assign(buf, buf + n);
                return true;
            }
            if (n == 0) {
                chunk->clear();
                return false;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                std::this_thread::sleep_for(std::chrono::milliseconds(poll_ms));
                waited += poll_ms;
                continue;
            }
            set_error(error, "read from interactive shell failed");
            return false;
        }
        chunk->clear();
        return false;
#endif
    }

    bool SshCliTransportProcess::send_signal(const std::string & signal_name,
                                             std::string * error)
    {
#if defined(_WIN32)
        (void)signal_name;
        set_error(error, "interactive process transport is not implemented on Windows yet");
        return false;
#else
        if (shell_child_pid_ <= 0) {
            set_error(error, "interactive shell process is not active");
            return false;
        }

        int sig = 0;
        if (signal_name == "INT" || signal_name == "SIGINT") {
            sig = SIGINT;
        } else if (signal_name == "TERM" || signal_name == "SIGTERM") {
            sig = SIGTERM;
        } else if (signal_name == "KILL" || signal_name == "SIGKILL") {
            sig = SIGKILL;
        } else {
            set_error(error, "unsupported signal name");
            return false;
        }

        if (kill(shell_child_pid_, sig) != 0) {
            set_error(error, "failed to signal interactive shell process");
            return false;
        }
        return true;
#endif
    }

    bool SshCliTransportProcess::is_shell_alive() const
    {
#if defined(_WIN32)
        return false;
#else
        if (!shell_open_ || shell_child_pid_ <= 0) {
            return false;
        }
        const int rc = kill(shell_child_pid_, 0);
        return rc == 0;
#endif
    }

    void SshCliTransportProcess::close()
    {
#if !defined(_WIN32)
        if (shell_stdin_fd_ >= 0) {
            ::close(shell_stdin_fd_);
            shell_stdin_fd_ = -1;
        }
        if (shell_stdout_fd_ >= 0) {
            ::close(shell_stdout_fd_);
            shell_stdout_fd_ = -1;
        }

        if (shell_child_pid_ > 0) {
            kill(shell_child_pid_, SIGTERM);
            int status = 0;
            bool exited = false;
            for (int i = 0; i < 20; ++i) {
                const pid_t rc = waitpid(shell_child_pid_, &status, WNOHANG);
                if (rc == shell_child_pid_ || (rc < 0 && errno == ECHILD)) {
                    exited = true;
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            if (!exited) {
                kill(shell_child_pid_, SIGKILL);
                waitpid(shell_child_pid_, &status, 0);
            }
            shell_child_pid_ = -1;
        }
#endif

        connected_ = false;
        authenticated_ = false;
        shell_open_ = false;
    }
}
