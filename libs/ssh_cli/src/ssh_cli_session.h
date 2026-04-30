#ifndef __LIBS_SSH_CLI_SESSION_H__
#define __LIBS_SSH_CLI_SESSION_H__

#include <cstdint>
#include <memory>
#include <string>

namespace yuan::libs::ssh_cli
{
    struct SshCliConnectionOptions;
    class SshCliTransport;

    enum class SshCliPhase
    {
        idle,
        connected,
        authenticated,
        shell_open
    };

    class SshCliSession
    {
    public:
        explicit SshCliSession(std::unique_ptr<SshCliTransport> transport);
        ~SshCliSession();

        bool connect(const SshCliConnectionOptions &options);
        bool authenticate_password(const std::string &password);
        bool authenticate_publickey(const std::string &private_key_path);
        bool open_shell();
        bool run_command(const std::string &command, std::string *stdout_data);
        bool read_stdout_chunk(std::string *chunk);
        bool send_signal(const std::string &signal_name);
        bool send_stdin(const std::string &chunk);
        bool is_shell_alive() const;
        void close();

        SshCliPhase phase() const;
        const std::string &last_error() const;

    private:
        void set_error(const std::string &message);

        std::unique_ptr<SshCliTransport> transport_;
        SshCliPhase phase_ = SshCliPhase::idle;
        std::string last_error_;
    };
}

#endif
