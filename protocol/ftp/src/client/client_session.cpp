#include "client/client_session.h"
#include "client/client_file_stream.h"
#include "common/def.h"
#include "common/file_stream_session.h"
#include "common/session.h"
#include "net/socket/inet_address.h"

#include <filesystem>
#include <iostream>
#include <sstream>
#include <vector>

namespace yuan::net::ftp
{
    namespace
    {
        InetAddress parse_pasv_endpoint(const std::string &body)
        {
            const auto left = body.find('(');
            const auto right = body.find(')');
            if (left == std::string::npos || right == std::string::npos || right <= left + 1) {
                return {};
            }
            std::string payload = body.substr(left + 1, right - left - 1);
            std::stringstream ss(payload);
            std::string item;
            std::vector<int> parts;
            while (std::getline(ss, item, ',')) {
                parts.push_back(std::stoi(item));
            }
            if (parts.size() != 6) {
                return {};
            }
            std::string ip = std::to_string(parts[0]) + "." + std::to_string(parts[1]) + "." + std::to_string(parts[2]) + "." + std::to_string(parts[3]);
            return {ip, parts[4] * 256 + parts[5]};
        }
    }

    ClientFtpSession::ClientFtpSession(Connection *conn, FtpApp *app, bool keepUtilSent) : FtpSession(conn, app, WorkMode::client, keepUtilSent) {}
    ClientFtpSession::~ClientFtpSession() {}

    void ClientFtpSession::on_opened(FtpFileStreamSession *fs)
    {
        std::cout << "client data opened action=" << static_cast<int>(client_context_.pending_action_)
                  << " stage=" << static_cast<int>(client_context_.transfer_stage_) << "\n";
        FtpSession::on_opened(fs);
        bool prepared = false;
        if (client_context_.pending_action_ == ClientPendingAction::list || client_context_.pending_action_ == ClientPendingAction::nlist) {
            prepared = prepare_list_receiver();
        } else if (client_context_.pending_action_ == ClientPendingAction::download) {
            prepared = prepare_download_file();
        } else if (client_context_.pending_action_ == ClientPendingAction::upload || client_context_.pending_action_ == ClientPendingAction::append) {
            prepared = prepare_upload_file();
        }

        std::cout << "client data prepared=" << prepared << " action=" << static_cast<int>(client_context_.pending_action_) << "\n";
        if (!prepared) {
            client_context_.reset_transfer_state();
            return;
        }

        bool sent = false;
        if (client_context_.pending_action_ == ClientPendingAction::list) {
            sent = send_command(client_context_.pending_path_.empty() ? "LIST\r\n" : "LIST " + client_context_.pending_path_ + "\r\n");
        } else if (client_context_.pending_action_ == ClientPendingAction::nlist) {
            sent = send_command(client_context_.pending_path_.empty() ? "NLST\r\n" : "NLST " + client_context_.pending_path_ + "\r\n");
        } else if (client_context_.pending_action_ == ClientPendingAction::download) {
            sent = send_command("RETR " + client_context_.pending_path_ + "\r\n");
        } else if (client_context_.pending_action_ == ClientPendingAction::upload) {
            sent = send_command("STOR " + client_context_.pending_path_ + "\r\n");
        } else if (client_context_.pending_action_ == ClientPendingAction::append) {
            sent = send_command("APPE " + client_context_.pending_path_ + "\r\n");
        }
        std::cout << "client transfer command sent=" << sent << " action=" << static_cast<int>(client_context_.pending_action_) << "\n";
        if (!sent) {
            client_context_.reset_transfer_state();
            return;
        }
        client_context_.transfer_stage_ = ClientTransferStage::wait_transfer;
    }

    void ClientFtpSession::on_read(Connection *conn)
    {
        auto buff = conn->get_input_buff(true);
        response_parser_.set_buff(buff);
        for (const auto &response : response_parser_.split_responses()) {
            client_context_.responses_.push_back(response);
            handle_response(response);
        }
    }

    void ClientFtpSession::on_completed(FtpFileStreamSession *fs)
    {
        (void)fs;
        auto *file = static_cast<FtpFileInfo *>(get_item_value<void *>("active_transfer_file"));
        if (file && file->in_memory_) {
            client_context_.list_output_ = file->memory_content_;
        }
        remove_item("active_transfer_file");
        FtpSession::on_completed(fs);
    }

    bool ClientFtpSession::login(const std::string &username, const std::string &password)
    {
        return send_command("USER " + username + "\r\n") && send_command("PASS " + password + "\r\n");
    }

    bool ClientFtpSession::list(const std::string &path) { return begin_pasv_flow(ClientPendingAction::list, path); }
    bool ClientFtpSession::nlist(const std::string &path) { return begin_pasv_flow(ClientPendingAction::nlist, path); }

    bool ClientFtpSession::download(const std::string &remote_path, const std::string &local_path)
    {
        namespace fs = std::filesystem;
        client_context_.pending_action_ = ClientPendingAction::download;
        client_context_.pending_path_ = remote_path;
        client_context_.transfer_local_path_ = local_path;
        std::size_t offset = 0;
        if (!local_path.empty() && fs::exists(local_path)) {
            offset = static_cast<std::size_t>(fs::file_size(local_path));
        }
        if (offset > 0) {
            set_item_value<int32_t>("restart_offset", static_cast<int32_t>(offset));
            client_context_.transfer_stage_ = ClientTransferStage::wait_rest;
            return send_command("REST " + std::to_string(offset) + "\r\n");
        }
        client_context_.transfer_stage_ = ClientTransferStage::wait_size;
        return send_command("SIZE " + remote_path + "\r\n");
    }

    bool ClientFtpSession::upload(const std::string &local_path, const std::string &remote_path)
    {
        namespace fs = std::filesystem;
        if (!fs::exists(local_path)) {
            return false;
        }
        client_context_.pending_action_ = ClientPendingAction::upload;
        client_context_.transfer_stage_ = ClientTransferStage::wait_allo;
        client_context_.pending_path_ = remote_path;
        client_context_.transfer_local_path_ = local_path;
        remove_item("restart_offset");
        return send_command("ALLO " + std::to_string(static_cast<std::size_t>(fs::file_size(local_path))) + "\r\n");
    }

    bool ClientFtpSession::append(const std::string &local_path, const std::string &remote_path)
    {
        namespace fs = std::filesystem;
        if (!fs::exists(local_path)) {
            return false;
        }
        client_context_.pending_action_ = ClientPendingAction::append;
        client_context_.transfer_stage_ = ClientTransferStage::wait_allo;
        client_context_.pending_path_ = remote_path;
        client_context_.transfer_local_path_ = local_path;
        remove_item("restart_offset");
        return send_command("ALLO " + std::to_string(static_cast<std::size_t>(fs::file_size(local_path))) + "\r\n");
    }

    void ClientFtpSession::handle_response(const FtpClientResponse &response)
    {
        std::cout << "client response code=" << response.code_ << " body=" << response.body_
                  << " action=" << static_cast<int>(client_context_.pending_action_)
                  << " stage=" << static_cast<int>(client_context_.transfer_stage_) << "\n";

        if (response.code_ == 350 && client_context_.pending_action_ == ClientPendingAction::download && client_context_.transfer_stage_ == ClientTransferStage::wait_rest) {
            client_context_.transfer_stage_ = ClientTransferStage::wait_size;
            send_command("SIZE " + client_context_.pending_path_ + "\r\n");
            return;
        }

        if (response.code_ == 213 && client_context_.pending_action_ == ClientPendingAction::download && client_context_.transfer_stage_ == ClientTransferStage::wait_size) {
            try {
                set_item_value<int32_t>("download_file_size", static_cast<int32_t>(std::stoll(response.body_)));
            } catch (...) {
                client_context_.reset_transfer_state();
                return;
            }
            client_context_.transfer_stage_ = ClientTransferStage::wait_pasv;
            send_command("PASV\r\n");
            return;
        }

        if (response.code_ == 200 && (client_context_.pending_action_ == ClientPendingAction::upload || client_context_.pending_action_ == ClientPendingAction::append) && client_context_.transfer_stage_ == ClientTransferStage::wait_allo) {
            client_context_.transfer_stage_ = ClientTransferStage::wait_pasv;
            send_command("PASV\r\n");
            return;
        }

        if (response.code_ == 227 && client_context_.transfer_stage_ == ClientTransferStage::wait_pasv) {
            const auto addr = parse_pasv_endpoint(response.body_);
            if (addr.get_port() <= 0) {
                client_context_.reset_transfer_state();
                return;
            }
            set_passive_addr(addr);
            start_file_stream(addr, (client_context_.pending_action_ == ClientPendingAction::upload || client_context_.pending_action_ == ClientPendingAction::append) ? StreamMode::Sender : StreamMode::Receiver);
            return;
        }

        // 只在传输完成阶段处理 226 响应
        if (response.code_ == 226 && client_context_.transfer_stage_ == ClientTransferStage::wait_transfer) {
            client_context_.reset_transfer_state();
            clear_passive_addr();
            remove_item("restart_offset");
            return;
        } else if (response.code_ >= 425 || response.code_ == 550) {
            remove_item("active_transfer_file");
            client_context_.reset_transfer_state();
            clear_passive_addr();
            remove_item("restart_offset");
            return;
        }
    }

    bool ClientFtpSession::begin_pasv_flow(ClientPendingAction action, const std::string &remote_path, const std::string &local_path)
    {
        client_context_.list_output_.clear();
        client_context_.pending_action_ = action;
        client_context_.transfer_stage_ = ClientTransferStage::wait_pasv;
        client_context_.pending_path_ = remote_path;
        client_context_.transfer_local_path_ = local_path;
        remove_item("restart_offset");
        return send_command("PASV\r\n");
    }

    bool ClientFtpSession::prepare_list_receiver()
    {
        FtpFileInfo info;
        info.mode_ = StreamMode::Receiver;
        info.type_ = FileType::directionary;
        info.in_memory_ = true;
        info.ready_ = true;
        get_file_manager()->reset();
        get_file_manager()->add_file(info);
        auto *file = get_file_manager()->get_next_file();
        if (!file) {
            return false;
        }
        set_item_value<void *>("active_transfer_file", file);
        return set_work_file(file);
    }

    bool ClientFtpSession::prepare_download_file()
    {
        namespace fs = std::filesystem;
        if (client_context_.transfer_local_path_.empty()) {
            client_context_.transfer_local_path_ = fs::path(client_context_.pending_path_).filename().generic_string();
        }
        const auto parent = fs::path(client_context_.transfer_local_path_).parent_path();
        if (!parent.empty()) {
            fs::create_directories(parent);
        }
        const std::size_t offset = static_cast<std::size_t>(get_item_value<int32_t>("restart_offset"));

        FtpFileInfo info;
        info.mode_ = StreamMode::Receiver;
        info.origin_name_ = client_context_.pending_path_;
        info.dest_name_ = client_context_.transfer_local_path_;
        info.file_size_ = static_cast<std::size_t>(get_item_value<int32_t>("download_file_size"));
        info.current_progress_ = offset;
        info.append_mode_ = offset > 0;
        info.ready_ = true;
        get_file_manager()->reset();
        get_file_manager()->add_file(info);
        auto *file = get_file_manager()->get_next_file();
        if (!file) {
            return false;
        }
        set_item_value<void *>("active_transfer_file", file);
        return set_work_file(file);
    }

    bool ClientFtpSession::prepare_upload_file()
    {
        namespace fs = std::filesystem;
        if (!fs::exists(client_context_.transfer_local_path_)) {
            return false;
        }
        const std::size_t offset = static_cast<std::size_t>(get_item_value<int32_t>("restart_offset"));
        const std::size_t total = static_cast<std::size_t>(fs::file_size(client_context_.transfer_local_path_));

        FtpFileInfo info;
        info.mode_ = StreamMode::Sender;
        info.origin_name_ = client_context_.transfer_local_path_;
        info.dest_name_ = client_context_.pending_path_;
        info.file_size_ = total;
        info.current_progress_ = offset;
        info.ready_ = true;
        get_file_manager()->reset();
        get_file_manager()->add_file(info);
        set_item_value<int32_t>("upload_file_size", static_cast<int32_t>(total - offset));
        auto *file = get_file_manager()->get_next_file();
        if (!file) {
            return false;
        }
        set_item_value<void *>("active_transfer_file", file);
        return set_work_file(file);
    }
}
