#ifndef __NET_SSH_SFTP_SSH_SFTP_SUBSYSTEM_H__
#define __NET_SSH_SFTP_SSH_SFTP_SUBSYSTEM_H__

#include "ssh_channel_handler.h"
#include "sftp/ssh_sftp_codec.h"
#include "sftp/ssh_file_system.h"
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace yuan::net::ssh
{
    using ::yuan::buffer::ByteBuffer;

    class SshSftpSubsystem : public SshChannelHandler
    {
    public:
        explicit SshSftpSubsystem(SshFileSystem *file_system);
        ~SshSftpSubsystem() override;

        void on_open(SshChannel *channel) override;
        void on_data(SshChannel *channel, const std::vector<uint8_t> &data) override;
        void on_eof(SshChannel *channel) override;
        void on_close(SshChannel *channel) override;
        void on_window_adjust(SshChannel *channel, uint32_t bytes_to_add) override;
        bool on_request(SshChannel *channel, const std::string &request_type,
                        const std::vector<uint8_t> &request_data) override;

    private:
        void process_packet(SshChannel *channel, const SftpPacket &packet);
        void send_status(SshChannel *channel, uint32_t request_id, SftpStatus code, const std::string &msg);
        void send_data_on_channel(SshChannel *channel, const ByteBuffer &buf);

        void handle_init(SshChannel *channel, const SftpPacket &packet);
        void handle_open(SshChannel *channel, const SftpPacket &packet);
        void handle_close(SshChannel *channel, const SftpPacket &packet);
        void handle_read(SshChannel *channel, const SftpPacket &packet);
        void handle_write(SshChannel *channel, const SftpPacket &packet);
        void handle_lstat(SshChannel *channel, const SftpPacket &packet);
        void handle_fstat(SshChannel *channel, const SftpPacket &packet);
        void handle_setstat(SshChannel *channel, const SftpPacket &packet);
        void handle_fsetstat(SshChannel *channel, const SftpPacket &packet);
        void handle_opendir(SshChannel *channel, const SftpPacket &packet);
        void handle_readdir(SshChannel *channel, const SftpPacket &packet);
        void handle_remove(SshChannel *channel, const SftpPacket &packet);
        void handle_mkdir(SshChannel *channel, const SftpPacket &packet);
        void handle_rmdir(SshChannel *channel, const SftpPacket &packet);
        void handle_realpath(SshChannel *channel, const SftpPacket &packet);
        void handle_stat(SshChannel *channel, const SftpPacket &packet);
        void handle_rename(SshChannel *channel, const SftpPacket &packet);
        void handle_readlink(SshChannel *channel, const SftpPacket &packet);
        void handle_symlink(SshChannel *channel, const SftpPacket &packet);
        void handle_extended(SshChannel *channel, const SftpPacket &packet);

        SshFileSystem *file_system_;
        SshChannel *channel_ = nullptr;
        bool version_exchanged_ = false;

        ByteBuffer recv_buf_;
    };
}

#endif
