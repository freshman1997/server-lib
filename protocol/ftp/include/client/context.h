#ifndef __NET_FTP_CLIENT_CONTEXT_H__
#define __NET_FTP_CLIENT_CONTEXT_H__

#include <string>
#include <vector>

namespace yuan::net::ftp
{
    enum class ClientPendingAction : char
    {
        none = 0,
        list,
        nlist,
        download,
        upload,
        append,
    };

    enum class ClientTransferStage : char
    {
        idle = 0,
        wait_size,
        wait_allo,
        wait_rest,
        wait_pasv,
        wait_transfer,
    };

    struct FtpClientResponse
    {
        int code_ = 0;
        std::string body_;
    };

    struct ClientContext
    {
        ClientPendingAction pending_action_ = ClientPendingAction::none;
        ClientTransferStage transfer_stage_ = ClientTransferStage::idle;
        std::string pending_path_;
        std::string transfer_local_path_;
        std::string list_output_;
        std::vector<FtpClientResponse> responses_;

        void reset_transfer_state()
        {
            pending_action_ = ClientPendingAction::none;
            transfer_stage_ = ClientTransferStage::idle;
            pending_path_.clear();
            transfer_local_path_.clear();
        }
    };
}

#endif
