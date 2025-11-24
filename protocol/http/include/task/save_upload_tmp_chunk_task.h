#ifndef __YUAN_NET_HTTP_SAVE_UPLOAD_TMP_CHUNK_H__
#define __YUAN_NET_HTTP_SAVE_UPLOAD_TMP_CHUNK_H__
#include <utility>

#include "define/upload.h"
#include "thread/runnable.h"

#include <memory>

namespace yuan::net::http
{
    class SaveUploadTempChunkTask final : public yuan::thread::Runnable
    {
    public:
        explicit SaveUploadTempChunkTask(UploadTmpChunk chunk) : tmp_chunk_(std::move(chunk)), upload_file_mapping_(nullptr) {}
        SaveUploadTempChunkTask() = default;
        ~SaveUploadTempChunkTask() override = default;

        void set_mapping(const std::shared_ptr<UploadFileMapping> &mapping)
        {
            upload_file_mapping_ = mapping;
        }

    protected:
        void run_internal() override;

    private:
        void do_merge_chunks() const;

    private:
        UploadTmpChunk tmp_chunk_;
        std::shared_ptr<UploadFileMapping> upload_file_mapping_;
    };
}

#endif // __YUAN_NET_HTTP_SAVE_UPLOAD_TMP_CHUNK_H__
