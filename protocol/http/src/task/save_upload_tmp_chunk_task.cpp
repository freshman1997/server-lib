#include "task/save_upload_tmp_chunk_task.h"
#include "buffer/pool.h"

#include <filesystem>
#include <fstream>
#include <iostream>

namespace yuan::net::http
{
    void SaveUploadTempChunkTask::run_internal()
    {
        if (!tmp_chunk_.begin_ || !tmp_chunk_.end_) {
            std::cerr << "empty body!!!" << std::endl;
            return;
        }

        std::ofstream of;
        of.open(tmp_chunk_.chunk_.tmp_file_, std::ios::binary);

        if (!of.good()) {
            std::cerr << "open file error: " << errno << std::endl;
            return;
        }

        const size_t bytesToWrite = tmp_chunk_.end_ - tmp_chunk_.begin_;
        of.write(tmp_chunk_.begin_, bytesToWrite);
        of.close();

        std::cout << "flush to disk, chunkIdx: " << tmp_chunk_.chunk_.idx_ << ", chunkSize: " << bytesToWrite << std::endl;
        buffer::BufferedPool::get_instance()->free(tmp_chunk_.buffer_);

        do_merge_chunks();
    }

    void SaveUploadTempChunkTask::do_merge_chunks() const
    {
        if (!upload_file_mapping_) {
            return;
        }

        std::ofstream finalFile;
        // 修复Windows和Linux的文件路径兼容性问题
        finalFile.open(std::filesystem::u8path(upload_file_mapping_->origin_file_name_), std::ios::binary);
        if (!finalFile.good()) {
            std::cerr << "write file error: " << errno << std::endl;
            return;
        }

        for (int i = 0; i < upload_file_mapping_->total_chunks_; i++) {
            std::ifstream tmpFile;
            tmpFile.open(upload_file_mapping_->chunks_[i].tmp_file_, std::ios::binary);
            if (!tmpFile.good()) {
                std::cerr << "open file error: " << errno << std::endl;
                std::filesystem::remove(upload_file_mapping_->origin_file_name_);
                return;
            }

            tmpFile.seekg(0, std::ios_base::end);
            std::size_t actualSize = tmpFile.tellg();
            tmpFile.seekg(0, std::ios_base::beg);

            std::vector<char> tmpFileData(actualSize);
            tmpFile.read(tmpFileData.data(), actualSize);

            if (std::size_t bytesRead = tmpFile.gcount(); bytesRead > 0) {
                finalFile.write(tmpFileData.data(), bytesRead);
            }
            tmpFile.close();
        }

        for (int i = 0; i < upload_file_mapping_->total_chunks_; i++) {
            std::filesystem::remove(upload_file_mapping_->chunks_[i].tmp_file_);
        }

        finalFile.close();

        std::cout << "merge chunks: " << upload_file_mapping_->total_chunks_ << ", size: " << upload_file_mapping_->file_size_ << " successfully." << std::endl;
    }
}
