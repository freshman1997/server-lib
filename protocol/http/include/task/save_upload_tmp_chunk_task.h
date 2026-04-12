#ifndef __YUAN_NET_HTTP_SAVE_UPLOAD_TMP_CHUNK_H__
#define __YUAN_NET_HTTP_SAVE_UPLOAD_TMP_CHUNK_H__

#include "define/upload.h"
#include "thread/runnable.h"

#include <memory>

namespace yuan::net::http
{
    class SaveUploadTempChunkTask final : public yuan::thread::Runnable
    {
    public:
        explicit SaveUploadTempChunkTask(UploadTmpChunk chunk)
            : tmp_chunk_(std::move(chunk)), session_(nullptr) {}
        
        SaveUploadTempChunkTask() = default;
        ~SaveUploadTempChunkTask() override = default;

        void set_session(const std::shared_ptr<UploadSession> &session) { session_ = session; }
        void set_mapping(const std::shared_ptr<UploadSession> &s) { session_ = s; }  // 向后兼容

    protected:
        void run_internal() override;

    private:
        // 保存单个分片到临时文件
        void save_single_chunk();
        // 合并所有分片为最终文件（仅在最后一个分片时调用）
        void merge_all_chunks();

    private:
        UploadTmpChunk tmp_chunk_;
        std::shared_ptr<UploadSession> session_;
    };
}

#endif
