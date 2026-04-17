#ifndef __NET_SMB_SMB_HANDLER_H__
#define __NET_SMB_SMB_HANDLER_H__

#include <cstdint>
#include <string>
#include <vector>

namespace yuan::net::smb
{
    class SmbSession;
    struct FileId;
    struct OpenFile;

    struct AuthInfo
    {
        std::string user_name;
        std::string domain_name;
    };

    struct CreateParams
    {
        uint32_t desired_access = 0;
        uint32_t file_attributes = 0;
        uint32_t share_access = 0;
        uint32_t create_disposition = 0;
        uint32_t create_options = 0;
    };

    class SmbHandler
    {
    public:
        virtual ~SmbHandler() = default;

        virtual bool on_authenticate(SmbSession *session, const std::string &user, const std::string &domain)
        {
            return true;
        }
        virtual bool on_tree_connect(SmbSession *session, const std::string &path)
        {
            return true;
        }
        virtual void on_tree_disconnect(SmbSession *session, uint32_t tree_id)
        {
        }
        virtual bool on_create(SmbSession *session, uint32_t tree_id, const std::string &path, const CreateParams &params)
        {
            return true;
        }
        virtual void on_close(SmbSession *session, const FileId &file_id)
        {
        }
        virtual bool on_read(SmbSession *session, const FileId &file_id, uint64_t offset, uint32_t length)
        {
            return true;
        }
        virtual bool on_write(SmbSession *session, const FileId &file_id, uint64_t offset, uint32_t length)
        {
            return true;
        }
        virtual bool on_query_directory(SmbSession *session, const FileId &file_id)
        {
            return true;
        }
        virtual bool on_query_info(SmbSession *session, const FileId &file_id)
        {
            return true;
        }
        virtual bool on_set_info(SmbSession *session, const FileId &file_id)
        {
            return true;
        }
        virtual bool on_rename(SmbSession *session, const FileId &file_id, const std::string &new_path)
        {
            return true;
        }
        virtual bool on_delete(SmbSession *session, const FileId &file_id)
        {
            return true;
        }
        virtual bool on_lock(SmbSession *session, const FileId &file_id)
        {
            return true;
        }
        virtual bool on_ioctl(SmbSession *session, const FileId &file_id, uint32_t ctl_code)
        {
            return true;
        }
        virtual bool on_pipe_open(SmbSession *session, const std::string &pipe_name)
        {
            return true;
        }
        virtual bool on_pipe_read(SmbSession *session, const FileId &file_id)
        {
            return true;
        }
        virtual bool on_pipe_write(SmbSession *session, const FileId &file_id)
        {
            return true;
        }
        virtual void on_session_opened(SmbSession *session)
        {
        }
        virtual void on_session_closed(SmbSession *session)
        {
        }
        virtual void on_logoff(SmbSession *session)
        {
        }
        virtual std::string on_dfs_resolve(SmbSession *session, const std::string &path)
        {
            return {};
        }
    };
}
#endif
