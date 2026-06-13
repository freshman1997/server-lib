#include "connection/ssh_terminal_bridge.h"

#include "protocol/ssh_message_codec.h"
#include "ssh_session.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include <array>
#include <chrono>
#include <cstring>
#include <cstdio>
#include <thread>

namespace yuan::net::ssh
{
    class SshNonPtyShellProcess
    {
    public:
        ~SshNonPtyShellProcess()
        {
            shutdown();
        }

        bool launch()
        {
#ifdef _WIN32
            return false;
#else
            int stdin_pipe[2] = {-1, -1};
            int stdout_pipe[2] = {-1, -1};
            int stderr_pipe[2] = {-1, -1};
            if (pipe(stdin_pipe) != 0 || pipe(stdout_pipe) != 0 || pipe(stderr_pipe) != 0) {
                close_pair(stdin_pipe);
                close_pair(stdout_pipe);
                close_pair(stderr_pipe);
                return false;
            }

            const pid_t pid = fork();
            if (pid < 0) {
                close_pair(stdin_pipe);
                close_pair(stdout_pipe);
                close_pair(stderr_pipe);
                return false;
            }
            if (pid == 0) {
                (void)dup2(stdin_pipe[0], STDIN_FILENO);
                (void)dup2(stdout_pipe[1], STDOUT_FILENO);
                (void)dup2(stderr_pipe[1], STDERR_FILENO);
                close_pair(stdin_pipe);
                close_pair(stdout_pipe);
                close_pair(stderr_pipe);
                const char *shell = std::getenv("SHELL");
                if (!shell || !*shell) {
                    shell = "/bin/sh";
                }
                execl(shell, shell, nullptr);
                execl("/bin/sh", "sh", nullptr);
                _exit(127);
            }

            child_pid_ = pid;
            stdin_fd_ = stdin_pipe[1];
            stdout_fd_ = stdout_pipe[0];
            stderr_fd_ = stderr_pipe[0];
            close_fd(stdin_pipe[0]);
            close_fd(stdout_pipe[1]);
            close_fd(stderr_pipe[1]);
            set_nonblocking(stdin_fd_);
            set_nonblocking(stdout_fd_);
            set_nonblocking(stderr_fd_);
            return true;
#endif
        }

        bool write_input(const std::vector<uint8_t> &data)
        {
#ifdef _WIN32
            (void)data;
            return false;
#else
            if (stdin_fd_ < 0 || data.empty()) {
                return false;
            }
            size_t offset = 0;
            while (offset < data.size()) {
                const ssize_t n = write(stdin_fd_, data.data() + offset, data.size() - offset);
                if (n > 0) {
                    offset += static_cast<size_t>(n);
                    continue;
                }
                if (n < 0 && (errno == EINTR)) {
                    continue;
                }
                if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                    pollfd pfd{};
                    pfd.fd = stdin_fd_;
                    pfd.events = POLLOUT;
                    if (poll(&pfd, 1, 50) <= 0) {
                        return offset > 0;
                    }
                    continue;
                }
                return offset > 0;
            }
            return true;
#endif
        }

        void close_input()
        {
#ifndef _WIN32
            close_fd(stdin_fd_);
#endif
        }

        bool read_stdout(std::vector<uint8_t> &out)
        {
#ifdef _WIN32
            out.clear();
            return false;
#else
            return read_available(stdout_fd_, out);
#endif
        }

        bool read_stderr(std::vector<uint8_t> &out)
        {
#ifdef _WIN32
            out.clear();
            return false;
#else
            return read_available(stderr_fd_, out);
#endif
        }

        int stdout_fd() const noexcept
        {
            return stdout_fd_;
        }

        int stderr_fd() const noexcept
        {
            return stderr_fd_;
        }

        bool poll_exit(int &exit_code)
        {
#ifdef _WIN32
            (void)exit_code;
            return false;
#else
            if (child_pid_ <= 0) {
                return false;
            }
            int status = 0;
            const pid_t rc = waitpid(child_pid_, &status, WNOHANG);
            if (rc == 0) {
                return false;
            }
            if (rc < 0) {
                return false;
            }
            child_pid_ = -1;
            if (WIFEXITED(status)) {
                exit_code = WEXITSTATUS(status);
            } else if (WIFSIGNALED(status)) {
                exit_code = 128 + WTERMSIG(status);
            } else {
                exit_code = 1;
            }
            return true;
#endif
        }

        void shutdown()
        {
#ifndef _WIN32
            close_fd(stdin_fd_);
            close_fd(stdout_fd_);
            close_fd(stderr_fd_);
            if (child_pid_ > 0) {
                kill(child_pid_, SIGTERM);
                int status = 0;
                (void)waitpid(child_pid_, &status, 0);
                child_pid_ = -1;
            }
#endif
        }

    private:
#ifndef _WIN32
        static void close_fd(int &fd)
        {
            if (fd >= 0) {
                close(fd);
                fd = -1;
            }
        }

        static void close_pair(int fds[2])
        {
            close_fd(fds[0]);
            close_fd(fds[1]);
        }

        static void set_nonblocking(int fd)
        {
            if (fd < 0) {
                return;
            }
            const int flags = fcntl(fd, F_GETFL, 0);
            if (flags >= 0) {
                (void)fcntl(fd, F_SETFL, flags | O_NONBLOCK);
            }
        }

        static bool read_available(int fd, std::vector<uint8_t> &out)
        {
            out.clear();
            if (fd < 0) {
                return false;
            }
            std::array<uint8_t, 4096> buffer{};
            while (true) {
                const ssize_t n = read(fd, buffer.data(), buffer.size());
                if (n > 0) {
                    out.insert(out.end(), buffer.begin(), buffer.begin() + n);
                    if (n < static_cast<ssize_t>(buffer.size())) {
                        return true;
                    }
                    continue;
                }
                if (n == 0) {
                    return !out.empty();
                }
                if (errno == EINTR) {
                    continue;
                }
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    return !out.empty();
                }
                return !out.empty();
            }
        }

        pid_t child_pid_ = -1;
        int stdin_fd_ = -1;
        int stdout_fd_ = -1;
        int stderr_fd_ = -1;
#else
        int child_pid_ = -1;
        int stdin_fd_ = -1;
        int stdout_fd_ = -1;
        int stderr_fd_ = -1;
#endif
    };

    namespace
    {
        SshChannel *resolve_recipient_channel(SshConnectionManager *conn_mgr, uint32_t recipient_channel)
        {
            if (!conn_mgr) {
                return nullptr;
            }
            auto *channel = conn_mgr->find_channel(recipient_channel);
            return channel ? channel : conn_mgr->find_channel_by_remote(recipient_channel);
        }

#ifdef _WIN32
        bool drain_pipe_available(HANDLE pipe_handle, std::vector<uint8_t> & out, bool force_drain)
        {
            std::array<uint8_t, 4096> buffer{};
            bool read_any = false;

            while (true) {
                DWORD available = 0;
                if (!PeekNamedPipe(pipe_handle, nullptr, 0, nullptr, &available, nullptr)) {
                    return read_any;
                }

                if (!force_drain && available == 0) {
                    return read_any;
                }

                const DWORD request = (available > 0 && available < static_cast<DWORD>(buffer.size()))
                                          ? available
                                          : static_cast<DWORD>(buffer.size());
                DWORD read_bytes = 0;
                if (!ReadFile(pipe_handle, buffer.data(), request, &read_bytes, nullptr)) {
                    const DWORD err = GetLastError();
                    if (err == ERROR_BROKEN_PIPE) {
                        return read_any;
                    }
                    return read_any;
                }

                if (read_bytes == 0) {
                    return read_any;
                }

                out.insert(out.end(), buffer.begin(), buffer.begin() + read_bytes);
                read_any = true;

                if (!force_drain && available <= read_bytes) {
                    return read_any;
                }
            }
        }

        bool run_windows_exec_capture(const std::string & command,
                                      std::vector<uint8_t> & stdout_out,
                                      std::vector<uint8_t> & stderr_out,
                                      uint32_t & exit_code)
        {
            stdout_out.clear();
            stderr_out.clear();
            exit_code = 1;

            if (command.empty()) {
                return false;
            }

            SECURITY_ATTRIBUTES sa{};
            sa.nLength = sizeof(sa);
            sa.bInheritHandle = TRUE;

            HANDLE stdout_read = nullptr;
            HANDLE stdout_write = nullptr;
            HANDLE stderr_read = nullptr;
            HANDLE stderr_write = nullptr;

            if (!CreatePipe(&stdout_read, &stdout_write, &sa, 0)) {
                return false;
            }
            if (!CreatePipe(&stderr_read, &stderr_write, &sa, 0)) {
                CloseHandle(stdout_read);
                CloseHandle(stdout_write);
                return false;
            }

            (void)SetHandleInformation(stdout_read, HANDLE_FLAG_INHERIT, 0);
            (void)SetHandleInformation(stderr_read, HANDLE_FLAG_INHERIT, 0);

            STARTUPINFOA si{};
            si.cb = sizeof(si);
            si.dwFlags = STARTF_USESTDHANDLES;
            si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
            si.hStdOutput = stdout_write;
            si.hStdError = stderr_write;

            PROCESS_INFORMATION pi{};
            std::string cmdline = "cmd.exe /C \"" + command + "\"";

            const BOOL created = CreateProcessA(
                nullptr,
                cmdline.data(),
                nullptr,
                nullptr,
                TRUE,
                CREATE_NO_WINDOW,
                nullptr,
                nullptr,
                &si,
                &pi);

            CloseHandle(stdout_write);
            CloseHandle(stderr_write);

            if (!created) {
                CloseHandle(stdout_read);
                CloseHandle(stderr_read);
                return false;
            }

            bool process_done = false;
            while (!process_done) {
                (void)drain_pipe_available(stdout_read, stdout_out, false);
                (void)drain_pipe_available(stderr_read, stderr_out, false);

                const DWORD wait_rc = WaitForSingleObject(pi.hProcess, 5);
                if (wait_rc == WAIT_OBJECT_0) {
                    process_done = true;
                }
            }

            (void)drain_pipe_available(stdout_read, stdout_out, true);
            (void)drain_pipe_available(stderr_read, stderr_out, true);

            DWORD child_exit = 1;
            if (GetExitCodeProcess(pi.hProcess, &child_exit) != 0) {
                exit_code = child_exit;
            }

            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
            CloseHandle(stdout_read);
            CloseHandle(stderr_read);
            return true;
        }
#endif
    }

    SshTerminalBridge::SshTerminalBridge(SshSession * session,
                                          SshConnectionManager * conn_mgr)
        : session_(session), conn_mgr_(conn_mgr)
    {
    }

    SshTerminalBridge::~SshTerminalBridge()
    {
        shutdown_all_pty_processes();
    }

    void SshTerminalBridge::register_pty_process(uint32_t channel_remote_id,
                                                 std::unique_ptr<SshPtyProcess> process)
    {
        if (!process) {
            return;
        }
        std::lock_guard<std::mutex> lock(pty_mutex_);
        pty_processes_[channel_remote_id] = std::move(process);
    }

    bool SshTerminalBridge::has_pty_process(uint32_t channel_remote_id) const
    {
        std::lock_guard<std::mutex> lock(pty_mutex_);
        auto it = pty_processes_.find(channel_remote_id);
        return it != pty_processes_.end() && it->second && it->second->ready();
    }

    bool SshTerminalBridge::has_any_pty_processes() const
    {
        std::lock_guard<std::mutex> lock(pty_mutex_);
        return !pty_processes_.empty() || !shell_processes_.empty();
    }

    bool SshTerminalBridge::has_any_client_pty_processes() const
    {
        std::vector<uint32_t> channel_ids;
        {
            std::lock_guard<std::mutex> lock(pty_mutex_);
            channel_ids.reserve(pty_processes_.size());
            for (const auto &entry : pty_processes_) {
                if (entry.second && entry.second->ready()) {
                    channel_ids.push_back(entry.first);
                }
            }
        }

        for (uint32_t channel_id : channel_ids) {
            auto *channel = conn_mgr_ ? conn_mgr_->find_channel_by_remote(channel_id) : nullptr;
            if (channel && channel->terminal_session_state().has_pty_request) {
                return true;
            }
        }
        return false;
    }

    int SshTerminalBridge::first_pty_master_fd() const
    {
        std::lock_guard<std::mutex> lock(pty_mutex_);
        for (const auto &entry : pty_processes_) {
            if (entry.second && entry.second->ready()) {
                return entry.second->backend().master_fd();
            }
        }
        return -1;
    }

    std::vector<int> SshTerminalBridge::terminal_output_fds() const
    {
        std::vector<int> fds;
        std::lock_guard<std::mutex> lock(pty_mutex_);
        fds.reserve(pty_processes_.size() + shell_processes_.size() * 2);
        for (const auto &entry : pty_processes_) {
            if (entry.second && entry.second->ready()) {
                const int fd = entry.second->backend().master_fd();
                if (fd >= 0) {
                    fds.push_back(fd);
                }
            }
        }
        for (const auto &entry : shell_processes_) {
            if (!entry.second) {
                continue;
            }
            const int stdout_fd = entry.second->stdout_fd();
            const int stderr_fd = entry.second->stderr_fd();
            if (stdout_fd >= 0) {
                fds.push_back(stdout_fd);
            }
            if (stderr_fd >= 0 && stderr_fd != stdout_fd) {
                fds.push_back(stderr_fd);
            }
        }
        return fds;
    }

    bool SshTerminalBridge::pump_pty_once(uint32_t channel_remote_id, SshHandler * handler)
    {
        SshPtyProcess *process = nullptr;
        {
            std::lock_guard<std::mutex> lock(pty_mutex_);
            auto it = pty_processes_.find(channel_remote_id);
            if (it == pty_processes_.end() || !it->second) {
                return false;
            }
            process = &*it->second;
        }

        auto *channel = conn_mgr_ ? conn_mgr_->find_channel_by_remote(channel_remote_id) : nullptr;
        if (!channel) {
            shutdown_pty_for_channel(channel_remote_id);
            return false;
        }

        bool pumped = false;
        std::vector<uint8_t> output;
        if (process->read_output(&output, 64 * 1024) && !output.empty()) {
            constexpr int kCoalesceRounds = 0;
            constexpr auto kCoalesceSleep = std::chrono::milliseconds(0);
            for (int i = 0; i < kCoalesceRounds && output.size() < 64 * 1024; ++i) {
                std::this_thread::sleep_for(kCoalesceSleep);
                std::vector<uint8_t> more;
                if (!process->read_output(&more, 64 * 1024 - output.size()) || more.empty()) {
                    break;
                }
                output.insert(output.end(), more.begin(), more.end());
            }
            session_->enqueue_outgoing(conn_mgr_->build_channel_data(channel->remote_id(), output));
            pumped = true;
        }

        SshPtyExitState exit_state;
        if (process->poll_exit(&exit_state) && channel->mark_termination_notified()) {
            output.clear();
            if (process->read_output(&output, 64 * 1024) && !output.empty()) {
                session_->enqueue_outgoing(conn_mgr_->build_channel_data(channel->remote_id(), output));
            }

            auto *effective_handler = handler ? handler : &SshHandler::default_handler();
            auto exit_info = effective_handler->on_command_exit(session_, channel);
            if (exit_state.signaled) {
                std::string sig_name = exit_info.signal_name.empty()
                                           ? ("SIG" + std::to_string(exit_state.term_signal))
                                           : exit_info.signal_name;
                session_->enqueue_outgoing(conn_mgr_->build_channel_exit_signal(channel->remote_id(),
                                                                                 sig_name,
                                                                                 false,
                                                                                 exit_info.error_message,
                                                                                 exit_info.language_tag));
            } else {
                const uint32_t code = exit_state.exited && exit_state.exit_code >= 0
                                          ? static_cast<uint32_t>(exit_state.exit_code)
                                          : exit_info.exit_status;
                session_->enqueue_outgoing(conn_mgr_->build_channel_exit_status(channel->remote_id(), code));
            }
            session_->enqueue_outgoing(conn_mgr_->build_channel_eof(channel->remote_id()));
            session_->enqueue_outgoing(conn_mgr_->build_channel_close(channel->remote_id()));
            shutdown_pty_for_channel(channel_remote_id);
            return true;
        }

        return pumped;
    }

    bool SshTerminalBridge::pump_all_pty_once(SshHandler * handler)
    {
        std::vector<uint32_t> channel_ids;
        {
            std::lock_guard<std::mutex> lock(pty_mutex_);
            channel_ids.reserve(pty_processes_.size());
            for (const auto &entry : pty_processes_) {
                channel_ids.push_back(entry.first);
            }
        }

        bool pumped = false;
        for (uint32_t channel_id : channel_ids) {
            pumped = pump_pty_once(channel_id, handler) || pumped;
        }

        std::vector<uint32_t> shell_channel_ids;
        {
            std::lock_guard<std::mutex> lock(pty_mutex_);
            shell_channel_ids.reserve(shell_processes_.size());
            for (const auto &entry : shell_processes_) {
                shell_channel_ids.push_back(entry.first);
            }
        }
        for (uint32_t channel_id : shell_channel_ids) {
            SshNonPtyShellProcess *process = nullptr;
            {
                std::lock_guard<std::mutex> lock(pty_mutex_);
                auto it = shell_processes_.find(channel_id);
                if (it == shell_processes_.end() || !it->second) {
                    continue;
                }
                process = &*it->second;
            }
            auto *channel = conn_mgr_ ? conn_mgr_->find_channel_by_remote(channel_id) : nullptr;
            if (!channel) {
                std::lock_guard<std::mutex> lock(pty_mutex_);
                shell_processes_.erase(channel_id);
                continue;
            }

            std::vector<uint8_t> output;
            if (process->read_stdout(output) && !output.empty()) {
                session_->enqueue_outgoing(conn_mgr_->build_channel_data(channel->remote_id(), output));
                pumped = true;
            }
            if (process->read_stderr(output) && !output.empty()) {
                session_->enqueue_outgoing(conn_mgr_->build_channel_extended_data(
                    channel->remote_id(),
                    static_cast<uint32_t>(SshChannelExtendedDataType::SSH_EXTENDED_DATA_STDERR),
                    output));
                pumped = true;
            }

            int exit_code = 0;
            if (process->poll_exit(exit_code) && channel->mark_termination_notified()) {
                if (process->read_stdout(output) && !output.empty()) {
                    session_->enqueue_outgoing(conn_mgr_->build_channel_data(channel->remote_id(), output));
                }
                if (process->read_stderr(output) && !output.empty()) {
                    session_->enqueue_outgoing(conn_mgr_->build_channel_extended_data(
                        channel->remote_id(),
                        static_cast<uint32_t>(SshChannelExtendedDataType::SSH_EXTENDED_DATA_STDERR),
                        output));
                }
                session_->enqueue_outgoing(conn_mgr_->build_channel_exit_status(channel->remote_id(), static_cast<uint32_t>(exit_code)));
                session_->enqueue_outgoing(conn_mgr_->build_channel_eof(channel->remote_id()));
                session_->enqueue_outgoing(conn_mgr_->build_channel_close(channel->remote_id()));
                std::lock_guard<std::mutex> lock(pty_mutex_);
                shell_processes_.erase(channel_id);
                pumped = true;
            }
        }
        return pumped;
    }

    void SshTerminalBridge::shutdown_pty_for_channel(uint32_t channel_remote_id)
    {
        std::lock_guard<std::mutex> lock(pty_mutex_);
        auto it = pty_processes_.find(channel_remote_id);
        if (it != pty_processes_.end() && it->second) {
            it->second->shutdown();
        }
        if (it != pty_processes_.end()) {
            pty_processes_.erase(it);
        }

        auto shell_it = shell_processes_.find(channel_remote_id);
        if (shell_it != shell_processes_.end() && shell_it->second) {
            shell_it->second->shutdown();
        }
        if (shell_it != shell_processes_.end()) {
            shell_processes_.erase(shell_it);
        }
    }

    void SshTerminalBridge::shutdown_all_pty_processes()
    {
        std::lock_guard<std::mutex> lock(pty_mutex_);
        for (auto &entry : pty_processes_) {
            if (entry.second) {
                entry.second->shutdown();
            }
        }
        pty_processes_.clear();
        for (auto &entry : shell_processes_) {
            if (entry.second) {
                entry.second->shutdown();
            }
        }
        shell_processes_.clear();
    }

    void SshTerminalBridge::handle_channel_data(const SshChannelDataMessage & msg, SshHandler * handler)
    {
        auto *channel = conn_mgr_ ? conn_mgr_->find_channel_by_remote(msg.recipient_channel) : nullptr;
        if (!channel) {
            channel = resolve_recipient_channel(conn_mgr_, msg.recipient_channel);
        }
        if (!channel) {
            return;
        }

        const uint32_t pty_channel_id = channel->remote_id();
        if (!has_pty_process(pty_channel_id)) {
            SshNonPtyShellProcess *shell = nullptr;
            {
                std::lock_guard<std::mutex> lock(pty_mutex_);
                auto it = shell_processes_.find(pty_channel_id);
                if (it != shell_processes_.end() && it->second) {
                    shell = &*it->second;
                }
            }
            if (shell) {
                (void)shell->write_input(msg.data);
            }
            return;
        }
        size_t total_written = 0;
        SshPtyProcess *process = nullptr;
        {
            std::lock_guard<std::mutex> lock(pty_mutex_);
            auto it = pty_processes_.find(pty_channel_id);
            if (it != pty_processes_.end() && it->second) {
                process = &*it->second;
            }
        }
        if (!process) {
            return;
        }

        std::vector<uint8_t> normalized;
        normalized.reserve(msg.data.size());
        for (size_t i = 0; i < msg.data.size(); ++i) {
            const uint8_t ch = msg.data[i];
            if (ch == '\r') {
                if (i + 1 < msg.data.size() && msg.data[i + 1] == '\n') {
                    ++i;
                }
                normalized.push_back('\n');
            } else {
                normalized.push_back(ch);
            }
        }

        const uint8_t *input = normalized.data();
        size_t remaining = normalized.size();
        while (remaining > 0) {
            size_t written = 0;
            if (!process->write_input(input, remaining, &written) || written == 0) {
                break;
            }
            input += written;
            remaining -= written;
            total_written += written;
        }

        (void)total_written;
#if defined(_WIN32)
        (void)pump_pty_once(pty_channel_id, handler);
#else
        pollfd pfd{};
        pfd.fd = process->backend().master_fd();
        pfd.events = POLLIN;
        for (int i = 0; i < 4; ++i) {
            pfd.revents = 0;
            const int timeout_ms = i == 0 ? 0 : 1;
            const int rc = poll(&pfd, 1, timeout_ms);
            if (rc <= 0 || (pfd.revents & (POLLIN | POLLHUP | POLLERR)) == 0) {
                break;
            }
            if (!pump_pty_once(pty_channel_id, handler)) {
                break;
            }
        }
#endif
    }

    void SshTerminalBridge::handle_channel_eof(const SshChannelEofMessage & msg)
    {
        auto *channel = conn_mgr_ ? conn_mgr_->find_channel_by_remote(msg.recipient_channel) : nullptr;
        if (!channel) {
            channel = resolve_recipient_channel(conn_mgr_, msg.recipient_channel);
        }
        if (!channel) {
            return;
        }
        std::lock_guard<std::mutex> lock(pty_mutex_);
        auto it = shell_processes_.find(channel->remote_id());
        if (it != shell_processes_.end() && it->second) {
            it->second->close_input();
        }
    }

    void SshTerminalBridge::handle_channel_request(const SshChannelRequestMessage & msg,
                                                   const yuan::buffer::ByteBuffer & response,
                                                   SshHandler * handler)
    {
        if (msg.request_type == "window-change") {
            on_pty_window_change_request(msg);
        } else if (msg.request_type == "signal") {
            on_pty_signal_request(msg);
        } else if (msg.request_type == "exec") {
            maybe_start_exec_pty_bridge(msg, response, handler);
        } else if (msg.request_type == "shell") {
            maybe_start_shell_pipe_bridge(msg, response);
            maybe_start_shell_pty_bridge(msg, response, handler);
        }
    }

    void SshTerminalBridge::on_pty_window_change_request(const SshChannelRequestMessage & msg)
    {
        auto *channel = conn_mgr_ ? conn_mgr_->find_channel_by_remote(msg.recipient_channel) : nullptr;
        if (!channel) {
            channel = resolve_recipient_channel(conn_mgr_, msg.recipient_channel);
        }
        if (!channel) {
            return;
        }

        SshPtyProcess *process = nullptr;
        {
            std::lock_guard<std::mutex> lock(pty_mutex_);
            auto it = pty_processes_.find(channel->remote_id());
            if (it != pty_processes_.end() && it->second) {
                process = &*it->second;
            }
        }
        if (!process || msg.request_specific_data.size() < 16) {
            return;
        }

        size_t offset = 0;
        const uint32_t width = SshMessageCodec::read_uint32(msg.request_specific_data.data(),
                                                            msg.request_specific_data.size(), offset);
        const uint32_t height = SshMessageCodec::read_uint32(msg.request_specific_data.data(),
                                                             msg.request_specific_data.size(), offset);
        const uint32_t pixel_width = SshMessageCodec::read_uint32(msg.request_specific_data.data(),
                                                                  msg.request_specific_data.size(), offset);
        const uint32_t pixel_height = SshMessageCodec::read_uint32(msg.request_specific_data.data(),
                                                                   msg.request_specific_data.size(), offset);
        std::string err;
        process->resize_terminal(width, height, pixel_width, pixel_height, &err);
    }

    void SshTerminalBridge::on_pty_signal_request(const SshChannelRequestMessage & msg)
    {
        auto *channel = resolve_recipient_channel(conn_mgr_, msg.recipient_channel);
        if (!channel) {
            return;
        }

        SshPtyProcess *process = nullptr;
        {
            std::lock_guard<std::mutex> lock(pty_mutex_);
            auto it = pty_processes_.find(channel->remote_id());
            if (it != pty_processes_.end() && it->second) {
                process = &*it->second;
            }
        }
        if (!process) {
            return;
        }

        size_t offset = 0;
        auto sig_name = SshMessageCodec::read_string(msg.request_specific_data.data(),
                                                     msg.request_specific_data.size(), offset);
        if (!sig_name) {
            return;
        }
        std::string err;
        process->send_signal(*sig_name, &err);
    }

    void SshTerminalBridge::maybe_start_shell_pipe_bridge(const SshChannelRequestMessage & msg,
                                                          const yuan::buffer::ByteBuffer & response)
    {
        if (response.readable_bytes() == 0) {
            return;
        }
        auto span = response.readable_span();
        const auto *span_bytes = reinterpret_cast<const uint8_t *>(span.data());
        const bool success = !span.empty() &&
                             span_bytes[0] == static_cast<uint8_t>(SshMessageType::SSH_MSG_CHANNEL_SUCCESS);
        auto *channel = resolve_recipient_channel(conn_mgr_, msg.recipient_channel);
        if (!success || !channel) {
            return;
        }

        auto &terminal_state = channel->terminal_session_state();
        if (terminal_state.has_pty_request || has_pty_process(channel->remote_id())) {
            return;
        }

        auto shell = std::make_unique<SshNonPtyShellProcess>();
        if (!shell->launch()) {
            return;
        }
        std::lock_guard<std::mutex> lock(pty_mutex_);
        shell_processes_[channel->remote_id()] = std::move(shell);
    }

    void SshTerminalBridge::maybe_start_shell_pty_bridge(const SshChannelRequestMessage & msg,
                                                          const yuan::buffer::ByteBuffer & response,
                                                          SshHandler * handler)
    {
        if (response.readable_bytes() == 0) {
            return;
        }
        auto span = response.readable_span();
        const auto *span_bytes = reinterpret_cast<const uint8_t *>(span.data());
        const bool success = !span.empty() &&
                             span_bytes[0] == static_cast<uint8_t>(SshMessageType::SSH_MSG_CHANNEL_SUCCESS);
        auto *channel = resolve_recipient_channel(conn_mgr_, msg.recipient_channel);
        if (!success || !channel) {
            return;
        }

        auto &terminal_state = channel->terminal_session_state();
        if (has_pty_process(channel->remote_id())) {
            return;
        }

        auto pty = std::make_unique<SshPtyProcess>();
        std::string err;
        if (pty->prepare(terminal_state.spec, &err) && pty->launch_shell("", true, &err)) {
            terminal_state.pty_bridge_active = true;
            const int master_fd = pty->backend().master_fd();
            register_pty_process(channel->remote_id(), std::move(pty));
            for (int i = 0; i < 32; ++i) {
                if (!pump_pty_once(channel->remote_id(), handler)) {
#ifndef _WIN32
                    pollfd pfd{};
                    pfd.fd = master_fd;
                    pfd.events = POLLIN;
                    (void)poll(&pfd, 1, 2);
#else
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
#endif
                    continue;
                }
            }
        } else {
            terminal_state.pty_bridge_active = false;
        }
    }

    void SshTerminalBridge::maybe_start_exec_pty_bridge(const SshChannelRequestMessage & msg,
                                                        const yuan::buffer::ByteBuffer & response,
                                                        SshHandler * handler)
    {
        auto *effective_handler = handler ? handler : &SshHandler::default_handler();
        if (!effective_handler->enable_builtin_exec_bridge()) {
            return;
        }

        if (response.readable_bytes() == 0) {
            return;
        }
        auto span = response.readable_span();
        const auto *span_bytes = reinterpret_cast<const uint8_t *>(span.data());
        const bool success = !span.empty() &&
                             span_bytes[0] == static_cast<uint8_t>(SshMessageType::SSH_MSG_CHANNEL_SUCCESS);
        auto *channel = resolve_recipient_channel(conn_mgr_, msg.recipient_channel);
        if (!success || !channel) {
            return;
        }

        auto &terminal_state = channel->terminal_session_state();
        if (!terminal_state.exec_requested || has_pty_process(channel->remote_id())) {
            return;
        }

#ifdef _WIN32
        std::vector<uint8_t> stdout_output;
        std::vector<uint8_t> stderr_output;
        uint32_t exit_code = 1;
        const bool executed = run_windows_exec_capture(terminal_state.exec_command,
                                                       stdout_output,
                                                       stderr_output,
                                                       exit_code);
        if (!executed) {
            const char *message = "Windows exec backend failed\n";
            stderr_output.assign(message, message + std::strlen(message));
            exit_code = 127;
        }
        if (!stdout_output.empty()) {
            session_->enqueue_outgoing(conn_mgr_->build_channel_data(channel->remote_id(), stdout_output));
        }
        if (!stderr_output.empty()) {
            session_->enqueue_outgoing(conn_mgr_->build_channel_extended_data(
                channel->remote_id(),
                static_cast<uint32_t>(SshChannelExtendedDataType::SSH_EXTENDED_DATA_STDERR),
                stderr_output));
        }
        channel->mark_termination_notified();
        session_->enqueue_outgoing(conn_mgr_->build_channel_exit_status(channel->remote_id(), exit_code));
        session_->enqueue_outgoing(conn_mgr_->build_channel_eof(channel->remote_id()));
        session_->enqueue_outgoing(conn_mgr_->build_channel_close(channel->remote_id()));
        return;
#endif

        auto pty = std::make_unique<SshPtyProcess>();
        std::string err;
        if (pty->prepare(terminal_state.spec, &err) && pty->launch_shell(terminal_state.exec_command, false, &err)) {
            terminal_state.pty_bridge_active = true;
            register_pty_process(channel->remote_id(), std::move(pty));
            pump_pty_once(channel->remote_id(), handler);
        } else {
            terminal_state.pty_bridge_active = false;
        }
    }
}
