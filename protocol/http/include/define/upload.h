#ifndef __YUAN_NET_HTTP_DEFINE_UPLOAD_H__
#define __YUAN_NET_HTTP_DEFINE_UPLOAD_H__
#include <string>
#include <unordered_map>

#include "buffer/buffer_reader.h"

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
        size_t begin_ = 0;
        size_t len_ = 0;
        UploadChunk chunk_;
        buffer::BufferReader reader_;
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
