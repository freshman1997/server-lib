#include "content/parsers/chunked_content_parser.h"
#include "content/types.h"
#include "content_type.h"
#include "packet.h"
#include "base/time.h"

#include <fstream>
#include <ios>
#include <string>
#include <utility>

namespace yuan::net::http 
{
    ChunkedContentParser::ChunkedContentParser() : exceed_save_file_(true)
    {
        max_chunk_size_ = 1024 * 1024 * 1024;
        exceed_chunk_size_save_file_ = 1024 * 1024;
        file_stream_ = nullptr;
        cur_chunk_size_ = 0;
    }
    
    ChunkedContentParser::~ChunkedContentParser()
    {
        reset();
    }

    void ChunkedContentParser::reset()
    {
        if (file_stream_) {
            file_stream_->flush();
            file_stream_->close();
            file_stream_ = nullptr;
        }

        rand_file_name_.clear();
        cur_chunk_size_ = 0;
    }

    std::size_t ChunkedContentParser::get_content_length()
    {
        return cur_chunk_size_;
    }

    bool ChunkedContentParser::can_parse(ContentType contentType)
    {
        return true;
    }

    static std::pair<int, ChunkState> read_chunk_length(buffer::BufferReader &reader, int &readLen)
    {
        int len = 0;
        auto state = ChunkState::invalid_chunck_;

        const size_t from = reader.get_read_offset();
        for (; reader; ++reader) {
            const char ch = *reader;
            if (ch == '\r') {
                state = ChunkState::completed_;
                break;
            }

            if (ch >= '0' && ch <= '9') {
                len = len * 16 + (ch - '0');
            } else if (ch >= 'a' && ch <= 'f') {
                len = len * 16 + (ch - 'a' + 10);
            } else if (ch >= 'A' && ch <= 'F') {
                len = len * 16 + (ch - 'A' + 10);
            }
        }

        readLen = reader.get_read_offset() - from;

        return {len, state};
    }

    // 解析
    ChunkState ChunkedContentParser::parse_chunked(HttpPacket *packet)
    {
        auto &reader = packet->get_buffer_reader();
        if (reader.readable_bytes() <= 0) {
            return ChunkState::invalid_chunck_;
        }

        auto resultState = ChunkState::invalid_chunck_;
        while (reader.readable_bytes() > 0) {
            int readLen = 0;
            const auto &p = read_chunk_length(reader, readLen);
            if (p.first < 0 || p.second != ChunkState::completed_ || readLen == 0) {
                resultState = p.second;
                break;
            }

            if (reader.get_read_offset() + 2 > reader.readable_bytes()) {
                resultState = ChunkState::need_more_;
                break;
            }

            reader.read_char();
            reader.read_char();

            if (!reader.skip_newline_symbol()) {
                resultState = ChunkState::internal_error_;
                break;
            }

            if (p.first <= 0) {
                break;
            }

            if (cur_chunk_size_ + p.first > max_chunk_size_) {
                resultState = ChunkState::invalid_chunck_;
                break;
            }

            cur_chunk_size_ += p.first;
            if (reader.get_read_offset() + 2 > reader.readable_bytes()) {
                resultState = ChunkState::need_more_;
                break;
            }

            if (!reader.skip_newline_symbol()) {
                resultState = ChunkState::internal_error_;
                break;
            }
        }

        std::string x_checksum, checksum;
        while (reader.readable_bytes() > 0) {
            if (!reader.skip_newline_symbol()) {
                resultState = ChunkState::completed_;
                break;
            }

            if (*reader != ':') {
                x_checksum.push_back(std::tolower(*reader));
            }
            else if (*reader != ' ')
            {
                checksum.push_back(std::tolower(*reader));
            }

            ++reader;
        }

        if (!x_checksum.empty() && x_checksum != "x-checksum") {
            if (x_checksum.size() < 10) {
                resultState = ChunkState::need_more_;
            }
        }

        bool wrote = false;
        if (resultState == ChunkState::completed_) {
            if (file_stream_) {
                if (reader.readable_bytes() > 0) {
                    reader.write(*file_stream_);
                    if (!file_stream_->good()) {
                        file_stream_->close();
                        delete file_stream_;
                        file_stream_ = nullptr;
                        return ChunkState::internal_error_;
                    }

                    file_stream_->flush();
                    wrote = true;
                }
            }

            packet->set_chunked_checksum(checksum);
        }

        if (cur_chunk_size_ >= exceed_chunk_size_save_file_ && !wrote) {
            if (!file_stream_) {
                rand_file_name_ = "___tmp___" + std::to_string(yuan::base::time::now());
                file_stream_ = new std::ofstream();
                file_stream_->open(rand_file_name_.c_str(), std::ios_base::app | std::ios_base::binary);
                if (!file_stream_->good()) {
                    return ChunkState::internal_error_;
                }
            }

            reader.write(*file_stream_);
            if (!file_stream_->good()) {
                file_stream_->close();
                delete file_stream_;
                file_stream_ = nullptr;
                return ChunkState::internal_error_;
            }
        }

        return resultState;
    }

    bool ChunkedContentParser::parse(HttpPacket *packet)
    {
        const ChunkState state = parse_chunked(packet);

        if (state == ChunkState::need_more_) {
            packet->set_body_state(BodyState::partial);
        }

        if (state == ChunkState::completed_) {
            auto *content = new Content(ContentType::chunked, nullptr);

            content->file_info_.tmp_file_name_ = rand_file_name_;
            content->file_info_.file_size_ = cur_chunk_size_;
            packet->set_body_content(content);
            packet->set_body_state(BodyState::fully);
        }

        return state == ChunkState::completed_;
    }
}