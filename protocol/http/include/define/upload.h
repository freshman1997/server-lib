#ifndef __YUAN_NET_HTTP_DEFINE_UPLOAD_H__
#define __YUAN_NET_HTTP_DEFINE_UPLOAD_H__

#include "buffer/byte_buffer.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace yuan::net::http
{
    // ============================================================
    // 单个已上传分片的元数据（存储�?UploadSession 中）
    // ============================================================
    struct UploadedChunk
    {
        int index = 0;
        uint64_t size = 0;
        std::string tmp_path;
    };

    // ============================================================
    // 一次分片上传任务的完整状�?    // ============================================================
    struct UploadSession
    {
        int total_chunks = 0;
        uint64_t total_size = 0;
        uint64_t created_at_ms = 0;
        uint64_t last_active_ms = 0;
        std::string filename;
        std::string upload_id;
        uint64_t chunk_size = 0;
        std::unordered_map<int, UploadedChunk> received;

        void touch(uint64_t now_ms)
        {
            if (created_at_ms == 0) {
                created_at_ms = now_ms;
            }
            last_active_ms = now_ms;
        }

        uint64_t received_bytes() const
        {
            uint64_t sum = 0;
            for (const auto &p : received) sum += p.second.size;
            return sum;
        }
        bool is_complete() const { return static_cast<int>(received.size()) == total_chunks; }
    };

    // 向后兼容别名
    using UploadFileMapping = UploadSession;

    // ============================================================
    // 待保存的分片数据（用于异步任务）
    // ============================================================
        struct UploadTmpChunk
    {
        UploadedChunk chunk_;

        // �첽����ǰ��ԭʼ��Ƭ���ݿ���
        ::yuan::buffer::ByteBuffer raw_buffer;

        const char *begin_ = nullptr;
        const char *end_   = nullptr;
        std::string tmp_file_;

        // �첽�����ύǰ�İ�ȫ����������Դ�������������ڱ仯
        std::vector<char> data_;
    };
}

#endif
