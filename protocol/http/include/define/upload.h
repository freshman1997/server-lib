#ifndef __YUAN_NET_HTTP_DEFINE_UPLOAD_H__
#define __YUAN_NET_HTTP_DEFINE_UPLOAD_H__
#include <cstdint>
#include <string>
#include <unordered_map>

#include "buffer/buffer.h"

namespace yuan::net::http
{
    struct UploadChunk
    {
        int idx_;
        uint64_t chunk_size_;
        std::string tmp_file_;
    };

    struct UploadTmpChunk
    {
        buffer::Buffer *buffer_ = nullptr;
        const char *begin_ = nullptr;
        const char *end_ = nullptr;
        UploadChunk chunk_;
    };

    struct UploadFileMapping
    {
        std::string origin_file_name_;
        uint64_t file_size_;
        int total_chunks_;
        std::unordered_map<int, UploadChunk> chunks_;
    };
}

#endif // __YUAN_NET_HTTP_DEFINE_UPLOAD_H__
