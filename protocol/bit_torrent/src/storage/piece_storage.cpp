#include "storage/piece_storage.h"

#include "utils.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <vector>

namespace yuan::net::bit_torrent
{

PieceStorage::~PieceStorage()
{
    close_all();
}

void PieceStorage::configure(const TorrentMeta *meta, const std::string &save_path)
{
    meta_ = meta;
    close_all();
    save_path_ = save_path;
    if (!meta_) {
        temp_file_prefix_.clear();
        save_path_.clear();
        return;
    }

    temp_file_prefix_ = save_path + "/" + meta_->info.name_ + ".partial.";
}

std::string PieceStorage::piece_file_path(int32_t piece_index) const
{
    return temp_file_prefix_ + std::to_string(piece_index);
}

int64_t PieceStorage::expected_piece_size(int32_t piece_index) const
{
    if (!meta_ || piece_index < 0 || piece_index >= meta_->info.piece_count()) {
        return -1;
    }

    const int64_t piece_offset = static_cast<int64_t>(piece_index) * meta_->info.piece_length_;
    return std::min<int64_t>(meta_->info.piece_length_, meta_->info.total_length_ - piece_offset);
}

FILE *PieceStorage::ensure_piece_file(int32_t piece_index)
{
    auto it = piece_files_.find(piece_index);
    if (it != piece_files_.end()) {
        return it->second;
    }

    auto *file = fopen(piece_file_path(piece_index).c_str(), "wb");
    if (!file) {
        return nullptr;
    }

    piece_files_[piece_index] = file;
    return file;
}

bool PieceStorage::write_piece(int32_t piece_index, uint32_t offset, const uint8_t *data, uint32_t length)
{
    if (!meta_ || piece_index < 0 || piece_index >= meta_->info.piece_count()) {
        return false;
    }

    auto *file = ensure_piece_file(piece_index);
    if (!file) {
        return false;
    }

    fseek(file, offset, SEEK_SET);
    const size_t written = fwrite(data, 1, length, file);
    fflush(file);
    return written == length;
}

bool PieceStorage::is_piece_complete(int32_t piece_index) const
{
    const int64_t expected_size = expected_piece_size(piece_index);
    if (expected_size <= 0) {
        return false;
    }

    FILE *file = fopen(piece_file_path(piece_index).c_str(), "rb");
    if (!file) {
        return false;
    }

    fseek(file, 0, SEEK_END);
    const long size = ftell(file);
    fclose(file);
    return size >= expected_size;
}

bool PieceStorage::verify_piece(int32_t piece_index) const
{
    if (!meta_ || piece_index < 0 || piece_index >= meta_->info.piece_count()) {
        return false;
    }

    const std::string expected_hash = meta_->info.piece_hash(piece_index);
    if (expected_hash.empty()) {
        return false;
    }

    FILE *file = fopen(piece_file_path(piece_index).c_str(), "rb");
    if (!file) {
        return false;
    }

    fseek(file, 0, SEEK_END);
    const long size = ftell(file);
    fseek(file, 0, SEEK_SET);
    const int64_t expected_size = expected_piece_size(piece_index);
    if (expected_size <= 0 || size != expected_size) {
        fclose(file);
        return false;
    }

    std::vector<uint8_t> piece_data(size);
    fread(piece_data.data(), 1, size, file);
    fclose(file);

    const auto hash = sha1_hash(piece_data.data(), piece_data.size());
    return hash.size() == expected_hash.size() &&
           std::memcmp(hash.data(), expected_hash.data(), hash.size()) == 0;
}

bool PieceStorage::verify_committed_piece(int32_t piece_index) const
{
    if (!meta_ || piece_index < 0 || piece_index >= meta_->info.piece_count())
    {
        return false;
    }

    const std::string expected_hash = meta_->info.piece_hash(piece_index);
    if (expected_hash.empty())
    {
        return false;
    }

    const auto piece_size = expected_piece_size(piece_index);
    if (piece_size <= 0)
    {
        return false;
    }

    std::vector<uint8_t> piece_data;
    if (!read_block(static_cast<uint32_t>(piece_index), 0, static_cast<uint32_t>(piece_size), piece_data))
    {
        return false;
    }

    const auto hash = sha1_hash(piece_data.data(), piece_data.size());
    return hash.size() == expected_hash.size() &&
           std::memcmp(hash.data(), expected_hash.data(), hash.size()) == 0;
}

std::vector<uint32_t> PieceStorage::scan_committed_pieces() const
{
    std::vector<uint32_t> pieces;
    if (!meta_)
    {
        return pieces;
    }

    const auto piece_count = meta_->info.piece_count();
    pieces.reserve(static_cast<size_t>(std::max(piece_count, 0)));
    for (int32_t piece_index = 0; piece_index < piece_count; ++piece_index)
    {
        if (verify_committed_piece(piece_index))
        {
            pieces.push_back(static_cast<uint32_t>(piece_index));
        }
    }
    return pieces;
}

std::vector<uint32_t> PieceStorage::restore_verified_partial_pieces()
{
    std::vector<uint32_t> restored;
    if (!meta_)
    {
        return restored;
    }

    const auto piece_count = meta_->info.piece_count();
    restored.reserve(static_cast<size_t>(std::max(piece_count, 0)));
    for (int32_t piece_index = 0; piece_index < piece_count; ++piece_index)
    {
        if (!is_piece_complete(piece_index) || !verify_piece(piece_index))
        {
            continue;
        }

        if (commit_piece(piece_index))
        {
            restored.push_back(static_cast<uint32_t>(piece_index));
        }
    }
    return restored;
}

bool PieceStorage::read_piece_data(int32_t piece_index, std::vector<uint8_t> &piece_data) const
{
    const int64_t expected_size = expected_piece_size(piece_index);
    if (expected_size <= 0)
    {
        return false;
    }

    FILE *file = fopen(piece_file_path(piece_index).c_str(), "rb");
    if (!file)
    {
        return false;
    }

    piece_data.resize(static_cast<size_t>(expected_size));
    const size_t read = fread(piece_data.data(), 1, piece_data.size(), file);
    fclose(file);
    return read == piece_data.size();
}

bool PieceStorage::write_piece_slice_to_file(const std::string &path,
                                             int64_t file_offset,
                                             const uint8_t *data,
                                             size_t length) const
{
    namespace fs = std::filesystem;

    const fs::path target_path(path);
    if (!target_path.parent_path().empty())
    {
        fs::create_directories(target_path.parent_path());
    }

    FILE *file = fopen(path.c_str(), "r+b");
    if (!file)
    {
        file = fopen(path.c_str(), "w+b");
    }
    if (!file)
    {
        return false;
    }

    fseek(file, file_offset, SEEK_SET);
    const size_t written = fwrite(data, 1, length, file);
    fflush(file);
    fclose(file);
    return written == length;
}

bool PieceStorage::read_piece_slice_from_file(const std::string &path,
                                              int64_t file_offset,
                                              uint8_t *data,
                                              size_t length) const
{
    FILE *file = fopen(path.c_str(), "rb");
    if (!file)
    {
        return false;
    }

    fseek(file, file_offset, SEEK_SET);
    const size_t read = fread(data, 1, length, file);
    fclose(file);
    return read == length;
}

bool PieceStorage::commit_piece(int32_t piece_index)
{
    if (!meta_ || !verify_piece(piece_index))
    {
        return false;
    }

    std::vector<uint8_t> piece_data;
    if (!read_piece_data(piece_index, piece_data))
    {
        return false;
    }

    const int64_t piece_start = static_cast<int64_t>(piece_index) * meta_->info.piece_length_;
    const int64_t piece_end = piece_start + static_cast<int64_t>(piece_data.size());

    namespace fs = std::filesystem;
    bool ok = true;

    if (meta_->info.files_.empty())
    {
        const auto target_path = (fs::path(save_path_) / meta_->info.name_).string();
        ok = write_piece_slice_to_file(target_path, piece_start, piece_data.data(), piece_data.size());
    }
    else
    {
        for (const auto &file : meta_->info.files_)
        {
            const int64_t file_start = file.offset_;
            const int64_t file_end = file.offset_ + file.length_;
            const int64_t overlap_start = std::max(piece_start, file_start);
            const int64_t overlap_end = std::min(piece_end, file_end);
            if (overlap_start >= overlap_end)
            {
                continue;
            }

            fs::path target_path = fs::path(save_path_) / meta_->info.name_;
            for (const auto &part : file.path_)
            {
                target_path /= part;
            }

            const auto piece_offset = static_cast<size_t>(overlap_start - piece_start);
            const auto file_offset = overlap_start - file_start;
            const auto length = static_cast<size_t>(overlap_end - overlap_start);
            if (!write_piece_slice_to_file(target_path.string(), file_offset, piece_data.data() + piece_offset, length))
            {
                ok = false;
                break;
            }
        }
    }

    if (ok)
    {
        close_piece_file(piece_index);
        std::remove(piece_file_path(piece_index).c_str());
    }

    return ok;
}

bool PieceStorage::read_block(uint32_t piece_index, uint32_t offset, uint32_t length, std::vector<uint8_t> &out) const
{
    if (!meta_)
    {
        return false;
    }

    const int64_t piece_size = expected_piece_size(static_cast<int32_t>(piece_index));
    if (piece_size <= 0 || offset >= static_cast<uint32_t>(piece_size) ||
        static_cast<int64_t>(offset) + static_cast<int64_t>(length) > piece_size)
    {
        return false;
    }

    out.assign(length, 0);
    const int64_t block_start = static_cast<int64_t>(piece_index) * meta_->info.piece_length_ + offset;
    const int64_t block_end = block_start + length;

    namespace fs = std::filesystem;
    size_t written = 0;

    if (meta_->info.files_.empty())
    {
        const auto target_path = (fs::path(save_path_) / meta_->info.name_).string();
        return read_piece_slice_from_file(target_path, block_start, out.data(), out.size());
    }

    for (const auto &file : meta_->info.files_)
    {
        const int64_t file_start = file.offset_;
        const int64_t file_end = file.offset_ + file.length_;
        const int64_t overlap_start = std::max(block_start, file_start);
        const int64_t overlap_end = std::min(block_end, file_end);
        if (overlap_start >= overlap_end)
        {
            continue;
        }

        fs::path target_path = fs::path(save_path_) / meta_->info.name_;
        for (const auto &part : file.path_)
        {
            target_path /= part;
        }

        const auto file_offset = overlap_start - file_start;
        const auto out_offset = static_cast<size_t>(overlap_start - block_start);
        const auto part_length = static_cast<size_t>(overlap_end - overlap_start);
        if (!read_piece_slice_from_file(target_path.string(), file_offset, out.data() + out_offset, part_length))
        {
            return false;
        }
        written += part_length;
    }

    return written == length;
}

void PieceStorage::close_piece_file(int32_t piece_index)
{
    auto it = piece_files_.find(piece_index);
    if (it == piece_files_.end())
    {
        return;
    }

    if (it->second)
    {
        fclose(it->second);
    }
    piece_files_.erase(it);
}

void PieceStorage::flush_all()
{
    for (auto &pair : piece_files_) {
        if (pair.second) {
            fflush(pair.second);
        }
    }
}

void PieceStorage::close_all()
{
    for (auto &pair : piece_files_) {
        if (pair.second) {
            fclose(pair.second);
            pair.second = nullptr;
        }
    }
    piece_files_.clear();
}

} // namespace yuan::net::bit_torrent
