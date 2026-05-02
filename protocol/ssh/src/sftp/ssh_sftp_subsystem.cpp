#include "sftp/ssh_sftp_subsystem.h"
#include "protocol/ssh_message_codec.h"
#include "connection/ssh_connection_manager.h"
#include "ssh_session.h"
#include "ssh_server.h"

namespace yuan::net::ssh
{
    static bool sftp_read_u32(const uint8_t * data, size_t len, size_t & offset, uint32_t & out)
    {
        if (offset + 4 > len)
            return false;
        out = (static_cast<uint32_t>(data[offset]) << 24) |
              (static_cast<uint32_t>(data[offset + 1]) << 16) |
              (static_cast<uint32_t>(data[offset + 2]) << 8) |
              static_cast<uint32_t>(data[offset + 3]);
        offset += 4;
        return true;
    }

    static bool sftp_read_u64(const uint8_t * data, size_t len, size_t & offset, uint64_t & out)
    {
        if (offset + 8 > len)
            return false;
        out = 0;
        for (int i = 0; i < 8; ++i) {
            out = (out << 8) | static_cast<uint64_t>(data[offset + i]);
        }
        offset += 8;
        return true;
    }

    static bool sftp_read_string(const uint8_t * data, size_t len, size_t & offset, std::string & out)
    {
        uint32_t slen = 0;
        if (!sftp_read_u32(data, len, offset, slen))
            return false;
        if (offset + slen > len)
            return false;
        out.assign(reinterpret_cast<const char *>(data + offset), slen);
        offset += slen;
        return true;
    }

    SshSftpSubsystem::SshSftpSubsystem(SshFileSystem * file_system)
        : file_system_(file_system)
    {
    }

    SshSftpSubsystem::~SshSftpSubsystem()
    {
    }

    void SshSftpSubsystem::on_open(SshChannel * channel)
    {
        channel_ = channel;
    }

    void SshSftpSubsystem::on_data(SshChannel * channel, const std::vector<uint8_t> & data)
    {
        recv_buf_.append(data.data(), data.size());

        while (recv_buf_.readable_bytes() >= 4) {
            size_t available = recv_buf_.readable_bytes();
            const uint8_t *ptr = reinterpret_cast<const uint8_t *>(recv_buf_.read_ptr());

            uint32_t packet_len = (static_cast<uint32_t>(ptr[0]) << 24) |
                                  (static_cast<uint32_t>(ptr[1]) << 16) |
                                  (static_cast<uint32_t>(ptr[2]) << 8) |
                                  static_cast<uint32_t>(ptr[3]);

            size_t total = 4 + packet_len;
            if (available < total)
                break;

            auto pkt = SshSftpCodec::decode(ptr, total);
            recv_buf_.consume(total);

            if (pkt) {
                process_packet(channel, *pkt);
            } else {
                return;
            }
        }
    }

    void SshSftpSubsystem::on_eof(SshChannel *)
    {
    }

    void SshSftpSubsystem::on_close(SshChannel *)
    {
        channel_ = nullptr;
    }

    void SshSftpSubsystem::on_window_adjust(SshChannel *, uint32_t)
    {
    }

    bool SshSftpSubsystem::on_request(SshChannel *, const std::string &, const std::vector<uint8_t> &)
    {
        return false;
    }

    void SshSftpSubsystem::process_packet(SshChannel * channel, const SftpPacket & packet)
    {
        if (!version_exchanged_ && packet.type != SftpPacketType::SSH_FXP_INIT) {
            return;
        }

        switch (packet.type) {
        case SftpPacketType::SSH_FXP_INIT:
            handle_init(channel, packet);
            break;
        case SftpPacketType::SSH_FXP_OPEN:
            handle_open(channel, packet);
            break;
        case SftpPacketType::SSH_FXP_CLOSE:
            handle_close(channel, packet);
            break;
        case SftpPacketType::SSH_FXP_READ:
            handle_read(channel, packet);
            break;
        case SftpPacketType::SSH_FXP_WRITE:
            handle_write(channel, packet);
            break;
        case SftpPacketType::SSH_FXP_LSTAT:
            handle_lstat(channel, packet);
            break;
        case SftpPacketType::SSH_FXP_FSTAT:
            handle_fstat(channel, packet);
            break;
        case SftpPacketType::SSH_FXP_SETSTAT:
            handle_setstat(channel, packet);
            break;
        case SftpPacketType::SSH_FXP_FSETSTAT:
            handle_fsetstat(channel, packet);
            break;
        case SftpPacketType::SSH_FXP_OPENDIR:
            handle_opendir(channel, packet);
            break;
        case SftpPacketType::SSH_FXP_READDIR:
            handle_readdir(channel, packet);
            break;
        case SftpPacketType::SSH_FXP_REMOVE:
            handle_remove(channel, packet);
            break;
        case SftpPacketType::SSH_FXP_MKDIR:
            handle_mkdir(channel, packet);
            break;
        case SftpPacketType::SSH_FXP_RMDIR:
            handle_rmdir(channel, packet);
            break;
        case SftpPacketType::SSH_FXP_REALPATH:
            handle_realpath(channel, packet);
            break;
        case SftpPacketType::SSH_FXP_STAT:
            handle_stat(channel, packet);
            break;
        case SftpPacketType::SSH_FXP_RENAME:
            handle_rename(channel, packet);
            break;
        case SftpPacketType::SSH_FXP_READLINK:
            handle_readlink(channel, packet);
            break;
        case SftpPacketType::SSH_FXP_SYMLINK:
            handle_symlink(channel, packet);
            break;
        case SftpPacketType::SSH_FXP_EXTENDED:
            handle_extended(channel, packet);
            break;
        default:
            send_status(channel, packet.request_id, SftpStatus::SSH_FX_OP_UNSUPPORTED, "Unsupported operation");
            break;
        }
    }

    void SshSftpSubsystem::send_status(SshChannel * channel, uint32_t request_id, SftpStatus code, const std::string & msg)
    {
        if (static_cast<uint32_t>(code) > static_cast<uint32_t>(SftpStatus::SSH_FX_OP_UNSUPPORTED)) {
            if (code == SftpStatus::SSH_FX_NO_SUCH_PATH ||
                code == SftpStatus::SSH_FX_INVALID_FILENAME ||
                code == SftpStatus::SSH_FX_NOT_A_DIRECTORY) {
                code = SftpStatus::SSH_FX_NO_SUCH_FILE;
            } else {
                code = SftpStatus::SSH_FX_FAILURE;
            }
        }
        auto buf = SshSftpCodec::encode_status(request_id, code, msg);
        send_data_on_channel(channel, buf);
    }

    void SshSftpSubsystem::send_data_on_channel(SshChannel * channel, const ByteBuffer & buf)
    {
        if (!channel || buf.readable_bytes() == 0)
            return;

        std::vector<uint8_t> sftp_data(reinterpret_cast<const uint8_t *>(buf.read_ptr()),
                                       reinterpret_cast<const uint8_t *>(buf.read_ptr()) + buf.readable_bytes());
        channel->enqueue_data(std::move(sftp_data));
    }

    void SshSftpSubsystem::handle_init(SshChannel * channel, const SftpPacket & packet)
    {
        uint32_t client_version = SFTP_DEFAULT_VERSION;
        if (packet.payload.size() >= 4) {
            client_version = (static_cast<uint32_t>(packet.payload[0]) << 24) |
                             (static_cast<uint32_t>(packet.payload[1]) << 16) |
                             (static_cast<uint32_t>(packet.payload[2]) << 8) |
                             static_cast<uint32_t>(packet.payload[3]);
        }

        uint32_t negotiated = std::min(client_version, SFTP_DEFAULT_VERSION);

        auto version_buf = SshSftpCodec::encode_version(negotiated);
        send_data_on_channel(channel, version_buf);
        version_exchanged_ = true;
    }

    void SshSftpSubsystem::handle_open(SshChannel * channel, const SftpPacket & packet)
    {
        size_t offset = 0;
        const uint8_t *data = packet.payload.data();
        size_t len = packet.payload.size();

        std::string path;
        if (!sftp_read_string(data, len, offset, path)) {
            send_status(channel, packet.request_id, SftpStatus::SSH_FX_BAD_MESSAGE, "Bad path");
            return;
        }

        uint32_t pflags = 0;
        if (!sftp_read_u32(data, len, offset, pflags)) {
            send_status(channel, packet.request_id, SftpStatus::SSH_FX_BAD_MESSAGE, "Bad pflags");
            return;
        }

        auto attrs = SshSftpCodec::decode_attrs(data, len, offset);

        auto result = file_system_->open(path, pflags, attrs.value_or(SftpFileAttrs{}));
        if (result.success) {
            auto buf = SshSftpCodec::encode_handle(packet.request_id, result.handle);
            send_data_on_channel(channel, buf);
        } else {
            send_status(channel, packet.request_id, result.status, result.status_message);
        }
    }

    void SshSftpSubsystem::handle_close(SshChannel * channel, const SftpPacket & packet)
    {
        size_t offset = 0;
        auto handle = SshSftpCodec::read_handle(packet.payload.data(), packet.payload.size(), offset);
        if (!handle) {
            send_status(channel, packet.request_id, SftpStatus::SSH_FX_INVALID_HANDLE, "Invalid handle");
            return;
        }

        auto result = file_system_->close(*handle);
        send_status(channel, packet.request_id, result.status,
                    result.success ? "OK" : result.status_message);
    }

    void SshSftpSubsystem::handle_read(SshChannel * channel, const SftpPacket & packet)
    {
        size_t offset = 0;
        const uint8_t *data = packet.payload.data();
        size_t len = packet.payload.size();

        auto handle = SshSftpCodec::read_handle(data, len, offset);
        if (!handle) {
            send_status(channel, packet.request_id, SftpStatus::SSH_FX_INVALID_HANDLE, "Invalid handle");
            return;
        }

        uint64_t read_offset = 0;
        if (!sftp_read_u64(data, len, offset, read_offset)) {
            send_status(channel, packet.request_id, SftpStatus::SSH_FX_BAD_MESSAGE, "Bad offset");
            return;
        }

        uint32_t read_len = 0;
        if (!sftp_read_u32(data, len, offset, read_len)) {
            send_status(channel, packet.request_id, SftpStatus::SSH_FX_BAD_MESSAGE, "Bad length");
            return;
        }

        auto result = file_system_->read(*handle, read_offset, read_len);
        if (result.success) {
            auto buf = SshSftpCodec::encode_data(packet.request_id, result.data.data(), result.data.size());
            send_data_on_channel(channel, buf);
        } else {
            send_status(channel, packet.request_id, result.status, result.status_message);
        }
    }

    void SshSftpSubsystem::handle_write(SshChannel * channel, const SftpPacket & packet)
    {
        size_t offset = 0;
        const uint8_t *data = packet.payload.data();
        size_t len = packet.payload.size();

        auto handle = SshSftpCodec::read_handle(data, len, offset);
        if (!handle) {
            send_status(channel, packet.request_id, SftpStatus::SSH_FX_INVALID_HANDLE, "Invalid handle");
            return;
        }

        uint64_t write_offset = 0;
        if (!sftp_read_u64(data, len, offset, write_offset)) {
            send_status(channel, packet.request_id, SftpStatus::SSH_FX_BAD_MESSAGE, "Bad offset");
            return;
        }

        uint32_t write_len = 0;
        if (!sftp_read_u32(data, len, offset, write_len)) {
            send_status(channel, packet.request_id, SftpStatus::SSH_FX_BAD_MESSAGE, "Bad length");
            return;
        }

        if (offset + write_len > len) {
            send_status(channel, packet.request_id, SftpStatus::SSH_FX_BAD_MESSAGE, "Write data length mismatch");
            return;
        }

        auto result = file_system_->write(*handle, write_offset, data + offset, write_len);
        send_status(channel, packet.request_id, result.status,
                    result.success ? "OK" : result.status_message);
    }

    void SshSftpSubsystem::handle_lstat(SshChannel * channel, const SftpPacket & packet)
    {
        size_t offset = 0;
        std::string path;
        if (!sftp_read_string(packet.payload.data(), packet.payload.size(), offset, path)) {
            send_status(channel, packet.request_id, SftpStatus::SSH_FX_BAD_MESSAGE, "Bad path");
            return;
        }

        auto result = file_system_->lstat(path);
        if (result.success) {
            auto buf = SshSftpCodec::encode_attrs(packet.request_id, result.attrs);
            send_data_on_channel(channel, buf);
        } else {
            send_status(channel, packet.request_id, result.status, result.status_message);
        }
    }

    void SshSftpSubsystem::handle_fstat(SshChannel * channel, const SftpPacket & packet)
    {
        size_t offset = 0;
        auto handle = SshSftpCodec::read_handle(packet.payload.data(), packet.payload.size(), offset);
        if (!handle) {
            send_status(channel, packet.request_id, SftpStatus::SSH_FX_INVALID_HANDLE, "Invalid handle");
            return;
        }

        auto result = file_system_->fstat(*handle);
        if (result.success) {
            auto buf = SshSftpCodec::encode_attrs(packet.request_id, result.attrs);
            send_data_on_channel(channel, buf);
        } else {
            send_status(channel, packet.request_id, result.status, result.status_message);
        }
    }

    void SshSftpSubsystem::handle_setstat(SshChannel * channel, const SftpPacket & packet)
    {
        size_t offset = 0;
        const uint8_t *data = packet.payload.data();
        size_t len = packet.payload.size();

        std::string path;
        if (!sftp_read_string(data, len, offset, path)) {
            send_status(channel, packet.request_id, SftpStatus::SSH_FX_BAD_MESSAGE, "Bad path");
            return;
        }

        auto attrs = SshSftpCodec::decode_attrs(data, len, offset);
        if (!attrs) {
            send_status(channel, packet.request_id, SftpStatus::SSH_FX_BAD_MESSAGE, "Bad attributes");
            return;
        }

        auto result = file_system_->setstat(path, *attrs);
        send_status(channel, packet.request_id, result.status,
                    result.success ? "OK" : result.status_message);
    }

    void SshSftpSubsystem::handle_fsetstat(SshChannel * channel, const SftpPacket & packet)
    {
        size_t offset = 0;
        auto handle = SshSftpCodec::read_handle(packet.payload.data(), packet.payload.size(), offset);
        if (!handle) {
            send_status(channel, packet.request_id, SftpStatus::SSH_FX_INVALID_HANDLE, "Invalid handle");
            return;
        }

        auto attrs = SshSftpCodec::decode_attrs(packet.payload.data(), packet.payload.size(), offset);
        if (!attrs) {
            send_status(channel, packet.request_id, SftpStatus::SSH_FX_BAD_MESSAGE, "Bad attributes");
            return;
        }

        auto result = file_system_->fsetstat(*handle, *attrs);
        send_status(channel, packet.request_id, result.status,
                    result.success ? "OK" : result.status_message);
    }

    void SshSftpSubsystem::handle_opendir(SshChannel * channel, const SftpPacket & packet)
    {
        size_t offset = 0;
        std::string path;
        if (!sftp_read_string(packet.payload.data(), packet.payload.size(), offset, path)) {
            send_status(channel, packet.request_id, SftpStatus::SSH_FX_BAD_MESSAGE, "Bad path");
            return;
        }

        auto result = file_system_->opendir(path);
        if (result.success) {
            auto buf = SshSftpCodec::encode_handle(packet.request_id, result.handle);
            send_data_on_channel(channel, buf);
        } else {
            send_status(channel, packet.request_id, result.status, result.status_message);
        }
    }

    void SshSftpSubsystem::handle_readdir(SshChannel * channel, const SftpPacket & packet)
    {
        size_t offset = 0;
        auto handle = SshSftpCodec::read_handle(packet.payload.data(), packet.payload.size(), offset);
        if (!handle) {
            send_status(channel, packet.request_id, SftpStatus::SSH_FX_INVALID_HANDLE, "Invalid handle");
            return;
        }

        auto result = file_system_->readdir(*handle);
        if (result.success) {
            auto buf = SshSftpCodec::encode_name(packet.request_id, result.entries);
            send_data_on_channel(channel, buf);
        } else {
            send_status(channel, packet.request_id, result.status, result.status_message);
        }
    }

    void SshSftpSubsystem::handle_remove(SshChannel * channel, const SftpPacket & packet)
    {
        size_t offset = 0;
        std::string path;
        if (!sftp_read_string(packet.payload.data(), packet.payload.size(), offset, path)) {
            send_status(channel, packet.request_id, SftpStatus::SSH_FX_BAD_MESSAGE, "Bad path");
            return;
        }

        auto result = file_system_->remove(path);
        send_status(channel, packet.request_id, result.status,
                    result.success ? "OK" : result.status_message);
    }

    void SshSftpSubsystem::handle_mkdir(SshChannel * channel, const SftpPacket & packet)
    {
        size_t offset = 0;
        const uint8_t *data = packet.payload.data();
        size_t len = packet.payload.size();

        std::string path;
        if (!sftp_read_string(data, len, offset, path)) {
            send_status(channel, packet.request_id, SftpStatus::SSH_FX_BAD_MESSAGE, "Bad path");
            return;
        }

        auto attrs = SshSftpCodec::decode_attrs(data, len, offset);
        auto result = file_system_->mkdir(path, attrs.value_or(SftpFileAttrs{}));
        send_status(channel, packet.request_id, result.status,
                    result.success ? "OK" : result.status_message);
    }

    void SshSftpSubsystem::handle_rmdir(SshChannel * channel, const SftpPacket & packet)
    {
        size_t offset = 0;
        std::string path;
        if (!sftp_read_string(packet.payload.data(), packet.payload.size(), offset, path)) {
            send_status(channel, packet.request_id, SftpStatus::SSH_FX_BAD_MESSAGE, "Bad path");
            return;
        }

        auto result = file_system_->rmdir(path);
        send_status(channel, packet.request_id, result.status,
                    result.success ? "OK" : result.status_message);
    }

    void SshSftpSubsystem::handle_realpath(SshChannel * channel, const SftpPacket & packet)
    {
        size_t offset = 0;
        std::string path;
        if (!sftp_read_string(packet.payload.data(), packet.payload.size(), offset, path)) {
            send_status(channel, packet.request_id, SftpStatus::SSH_FX_BAD_MESSAGE, "Bad path");
            return;
        }

        auto result = file_system_->realpath(path);
        if (result.success) {
            SftpNameEntry entry;
            entry.filename = result.path;
            entry.longname = result.path;
            entry.attrs = result.attrs;
            auto buf = SshSftpCodec::encode_name(packet.request_id, { entry });
            send_data_on_channel(channel, buf);
        } else {
            send_status(channel, packet.request_id, result.status, result.status_message);
        }
    }

    void SshSftpSubsystem::handle_stat(SshChannel * channel, const SftpPacket & packet)
    {
        size_t offset = 0;
        std::string path;
        if (!sftp_read_string(packet.payload.data(), packet.payload.size(), offset, path)) {
            send_status(channel, packet.request_id, SftpStatus::SSH_FX_BAD_MESSAGE, "Bad path");
            return;
        }

        auto result = file_system_->stat(path);
        if (result.success) {
            auto buf = SshSftpCodec::encode_attrs(packet.request_id, result.attrs);
            send_data_on_channel(channel, buf);
        } else {
            send_status(channel, packet.request_id, result.status, result.status_message);
        }
    }

    void SshSftpSubsystem::handle_rename(SshChannel * channel, const SftpPacket & packet)
    {
        size_t offset = 0;
        const uint8_t *data = packet.payload.data();
        size_t len = packet.payload.size();

        std::string old_path, new_path;
        if (!sftp_read_string(data, len, offset, old_path)) {
            send_status(channel, packet.request_id, SftpStatus::SSH_FX_BAD_MESSAGE, "Bad old path");
            return;
        }
        if (!sftp_read_string(data, len, offset, new_path)) {
            send_status(channel, packet.request_id, SftpStatus::SSH_FX_BAD_MESSAGE, "Bad new path");
            return;
        }

        uint32_t flags = 0;
        if (offset + 4 <= len) {
            sftp_read_u32(data, len, offset, flags);
        }

        auto result = file_system_->rename(old_path, new_path, flags);
        send_status(channel, packet.request_id, result.status,
                    result.success ? "OK" : result.status_message);
    }

    void SshSftpSubsystem::handle_readlink(SshChannel * channel, const SftpPacket & packet)
    {
        size_t offset = 0;
        std::string path;
        if (!sftp_read_string(packet.payload.data(), packet.payload.size(), offset, path)) {
            send_status(channel, packet.request_id, SftpStatus::SSH_FX_BAD_MESSAGE, "Bad path");
            return;
        }

        auto result = file_system_->readlink(path);
        if (result.success) {
            SftpNameEntry entry;
            entry.filename = result.link_target;
            entry.longname = result.link_target;
            entry.attrs = result.attrs;
            auto buf = SshSftpCodec::encode_name(packet.request_id, { entry });
            send_data_on_channel(channel, buf);
        } else {
            send_status(channel, packet.request_id, result.status, result.status_message);
        }
    }

    void SshSftpSubsystem::handle_symlink(SshChannel * channel, const SftpPacket & packet)
    {
        size_t offset = 0;
        const uint8_t *data = packet.payload.data();
        size_t len = packet.payload.size();

        std::string link_path, target_path;
        if (!sftp_read_string(data, len, offset, link_path)) {
            send_status(channel, packet.request_id, SftpStatus::SSH_FX_BAD_MESSAGE, "Bad link path");
            return;
        }
        if (!sftp_read_string(data, len, offset, target_path)) {
            send_status(channel, packet.request_id, SftpStatus::SSH_FX_BAD_MESSAGE, "Bad target path");
            return;
        }

        auto result = file_system_->symlink(link_path, target_path);
        send_status(channel, packet.request_id, result.status,
                    result.success ? "OK" : result.status_message);
    }

    void SshSftpSubsystem::handle_extended(SshChannel * channel, const SftpPacket & packet)
    {
        send_status(channel, packet.request_id, SftpStatus::SSH_FX_OP_UNSUPPORTED, "Extended operations not supported");
    }
}
