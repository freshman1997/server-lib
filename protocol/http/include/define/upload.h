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
    // 鍗曚釜宸蹭笂浼犲垎鐗囩殑鍏冩暟鎹紙瀛樺偍鍦?UploadSession 涓級
    // ============================================================
    struct UploadedChunk
    {
        int index = 0;
        uint64_t size = 0;
        std::string tmp_path;
    };

    // ============================================================
    // 涓�娆″垎鐗囦笂浼犱换鍔＄殑瀹屾暣鐘舵�?    // ============================================================
    struct UploadSession
    {
        int total_chunks = 0;
        uint64_t total_size = 0;
        std::string filename;
        std::string upload_id;
        std::unordered_map<int, UploadedChunk> received;

        uint64_t received_bytes() const
        {
            uint64_t sum = 0;
            for (const auto &p : received) sum += p.second.size;
            return sum;
        }
        bool is_complete() const { return static_cast<int>(received.size()) == total_chunks; }
    };

    // 鍚戝悗鍏煎鍒悕
    using UploadFileMapping = UploadSession;

    // ============================================================
    // 寰呬繚瀛樼殑鍒嗙墖鏁版嵁锛堢敤浜庡紓姝ヤ换鍔★級
    // ============================================================
        struct UploadTmpChunk
    {
        UploadedChunk chunk_;

        // 异步保存前的原始分片数据快照
        ::yuan::buffer::ByteBuffer raw_buffer;

        const char *begin_ = nullptr;
        const char *end_   = nullptr;
        std::string tmp_file_;

        // 异步任务提交前的安全副本，避免源缓冲区生命周期变化
        std::vector<char> data_;
    };
}

#endif
