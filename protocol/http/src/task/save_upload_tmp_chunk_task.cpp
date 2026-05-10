#include "task/save_upload_tmp_chunk_task.h"
#include "logger.h"
#include <string>
#include <vector>
#include <filesystem>
#include <fstream>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace yuan::net::http
{

    // 将 UTF-8 字符串转换为 std::wstring（Windows 上用于正确的中文文件名支持）
#ifdef _WIN32
    static std::wstring utf8_to_wide(const std::string &utf8str)
    {
        if (utf8str.empty()) return std::wstring();
        int len = MultiByteToWideChar(CP_UTF8, 0, utf8str.c_str(), -1, nullptr, 0);
        if (len == 0) return std::wstring();
        std::wstring wstr(static_cast<size_t>(len) - 1, 0);
        MultiByteToWideChar(CP_UTF8, 0, utf8str.c_str(), -1, &wstr[0], len);
        return wstr;
    }
#endif

    // 将 UTF-8 std::string 构造为 std::filesystem::path，确保 Windows 上正确处理中文文件名
    static std::filesystem::path utf8_path(const std::string &utf8str)
    {
#ifdef _WIN32
        return std::filesystem::path(utf8_to_wide(utf8str));
#else
        return std::filesystem::path(utf8str);
#endif
    }
    // ============================================================
    // 保存单个分片到临时文件    
    // ============================================================
    
    void SaveUploadTempChunkTask::save_single_chunk()
    {
        // 如果数据已经在文件中（parser阶段已落盘），无需再次保存
        if (!tmp_chunk_.chunk_.tmp_path.empty() && !tmp_chunk_.begin_ && tmp_chunk_.data_.empty()) {
            return;
        }

        // 使用预拷贝的数据（data_ 在 serve_upload 主线程中同步填充，线程安全）
        const char *data = nullptr;
        size_t data_size = 0;
        if (!tmp_chunk_.data_.empty()) {
            data = tmp_chunk_.data_.data();
            data_size = tmp_chunk_.data_.size();
        } else if (tmp_chunk_.begin_ && tmp_chunk_.end_ > tmp_chunk_.begin_) {
            data = tmp_chunk_.begin_;
            data_size = static_cast<size_t>(tmp_chunk_.end_ - tmp_chunk_.begin_);
        }

        if (!data || data_size == 0) {
            LOG_WARN("[Upload] empty chunk data for index={}", tmp_chunk_.chunk_.index);
            return;
        }

        // 确保目录存在
        std::error_code ec;
        auto tmp_path = utf8_path(tmp_chunk_.chunk_.tmp_path);
        auto parent_dir = tmp_path.parent_path();
        if (!parent_dir.empty() && !std::filesystem::exists(parent_dir)) {
            std::filesystem::create_directories(parent_dir, ec);
        }

        std::ofstream ofs(tmp_path, std::ios::binary);
        if (!ofs.good()) {
            LOG_ERROR("[Upload] failed to open temp file: {}", tmp_chunk_.chunk_.tmp_path);
            return;
        }

        ofs.write(data, data_size);
        ofs.close();

        LOG_INFO("[Upload] saved chunk {} ({} bytes) -> {}", tmp_chunk_.chunk_.index, data_size, tmp_chunk_.chunk_.tmp_path);

    }

    // ============================================================
    // 合并所有分片为最终文件（流式合并，64KB 缓冲区）
    // ============================================================
    
    void SaveUploadTempChunkTask::merge_all_chunks()
    {
        if (!session_) return;

        // 安全性：清理路径遍历
        std::string safe_name = session_->filename;
        auto last_slash = safe_name.find_last_of("/\\");
        if (last_slash != std::string::npos) safe_name = safe_name.substr(last_slash + 1);
        if (safe_name.find("..") != std::string::npos) safe_name = "uploaded_file";
        
        // Use utf8_path so Windows handles UTF-8 filenames correctly.
        const auto final_path = utf8_path("./uploads/" + safe_name);

        // 确保上传目录存在
        {
            std::error_code ec;
            std::filesystem::create_directories("./uploads", ec);
        }

        std::ofstream final_file(final_path, std::ios::binary);
        if (!final_file.good()) {
            LOG_ERROR("[Upload] cannot create: ./uploads/{}", safe_name);
            return;
        }

        constexpr size_t BUF_SIZE = 64 * 1024;
        std::vector<char> copy_buf(BUF_SIZE, 0);

        for (int i = 0; i < session_->total_chunks; ++i)
        {
            auto it = session_->received.find(i);
            if (it == session_->received.end()) {
                LOG_WARN("[Upload] missing chunk {}, merge aborted", i);
                final_file.close();
                std::filesystem::remove(final_path);
                return;
            }

            std::ifstream ifs(utf8_path(it->second.tmp_path), std::ios::binary);
            if (!ifs.good()) {
                LOG_ERROR("[Upload] cannot open chunk: {}", it->second.tmp_path);
                final_file.close();
                std::filesystem::remove(final_path);
                return;
            }

            while (ifs) {
                ifs.read(copy_buf.data(), BUF_SIZE);
                if (auto n = ifs.gcount(); n > 0)
                    final_file.write(copy_buf.data(), n);
            }
            ifs.close();
        }

        final_file.close();

        // 清理临时文件
        for (int i = 0; i < session_->total_chunks; ++i) {
            auto it = session_->received.find(i);
            if (it != session_->received.end()) {
                std::error_code ec;
                std::filesystem::remove(utf8_path(it->second.tmp_path), ec);
            }
        }
        
        // 清理空目录
        LOG_INFO("[Upload] merged {} chunks -> ./uploads/{}", session_->total_chunks, safe_name);
    }

    // ============================================================
    // 任务入口
    // ============================================================
    
    void SaveUploadTempChunkTask::run_internal()
    {
        save_single_chunk();
        if (session_ && !session_->filename.empty()) {
            merge_all_chunks();
        }
    }
}

