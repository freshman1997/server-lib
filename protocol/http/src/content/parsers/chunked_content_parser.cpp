#include "content/parsers/chunked_content_parser.h"
#include "buffer/pool.h"
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
        cached_buffer_ = nullptr;
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

        if (cached_buffer_) {
            buffer::BufferedPool::get_instance()->free(cached_buffer_);
            cached_buffer_ = nullptr;
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

    static std::pair<int, ChunkState> read_chunk_length(const char *begin, const char *end, int &readLen)
    {
        int len = 0;
        ChunkState state = ChunkState::invalid_chunck_;

        const char *from = begin;
        
        for (; begin != end; ++begin) {
            char ch = *begin;
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

        readLen = begin - from;

        return {len, state};
    }

    // 解析
    ChunkState ChunkedContentParser::parse_chunked(HttpPacket *packet)
    {
        buffer::Buffer *buffer = packet->get_buff(true, false);
        const char *begin = buffer->peek();
        const char *end = buffer->peek_end();
        if (!begin || !end || end - begin <= 0) {
            return ChunkState::invalid_chunck_;
        }

        ChunkState resultState = ChunkState::invalid_chunck_;
        buffer::Buffer *newBuffer = cached_buffer_ ? cached_buffer_ : packet->get_buff();
        while (begin != end) {
            int readLen = 0;
            int fromIdx = buffer->get_read_index();
            auto p = read_chunk_length(begin, end, readLen);
            if (p.first < 0 || p.second != ChunkState::completed_ || readLen == 0) {
                resultState = p.second;
                break;
            }

            begin += readLen;
            if (begin + 2 > end) {
                resultState = ChunkState::need_more_;
                buffer->reset_read_index(fromIdx);
                break;
            }

            buffer->add_read_index(2);

            if (*begin == '\r' && *(begin + 1) == '\n') {
                begin += 2;
            } else {
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
            buffer->add_read_index(p.first);
            newBuffer->write_string(begin, p.first);

            begin += p.first;
            if (begin + 2 > end) {
                resultState = ChunkState::need_more_;
                buffer->reset_read_index(fromIdx);
                break;
            }

            buffer->add_read_index(2);

            if (*begin == '\r' && *(begin + 1) == '\n') {
                begin += 2;
            } else {
                break;
            }
        }

        std::string x_checksum, checksum;
        while (begin != end) {
            if (*begin == '\r' && *(begin + 1) == '\n') {
                begin += 2;
                resultState = ChunkState::completed_;
                break;
            }

            if (*begin != ':') {
                x_checksum.push_back(std::tolower(*begin));
            }
            else if (*begin != ' ')
            {
                checksum.push_back(std::tolower(*begin));
            }

            ++begin;
        }

        if (!x_checksum.empty() && x_checksum != "x-checksum") {
            if (x_checksum.size() < 10) {
                resultState = ChunkState::need_more_;
            }
        }

        bool wrote = false;
        if (resultState == ChunkState::need_more_) {
            if (!cached_buffer_) {
                cached_buffer_ = packet->get_buff(true, false);
            }

            if (buffer->readable_bytes() > 0) {
                packet->get_buff()->append_buffer(*buffer);
            }
        } else if (resultState == ChunkState::completed_) {
            if (file_stream_) {
                if (cached_buffer_ && cached_buffer_->readable_bytes() > 0) {
                    file_stream_->write(cached_buffer_->peek(), cached_buffer_->readable_bytes());
                    if (!file_stream_->good()) {
                        file_stream_->close();
                        delete file_stream_;
                        file_stream_ = nullptr;
                        return ChunkState::internal_error_;
                    }

                    file_stream_->flush();
                    cached_buffer_->reset();
                    wrote = true;
                }
            }

            packet->set_chunked_checksum(checksum);
        }

        if (cur_chunk_size_ >= exceed_chunk_size_save_file_ && !wrote) {
            if (!file_stream_) {
                rand_file_name_ = "___tmp___" + std::to_string(yuan::base::time::now());
                file_stream_ = new std::fstream();
                file_stream_->open(rand_file_name_.c_str(), std::ios_base::app | std::ios_base::binary);
                if (!file_stream_->good()) {
                    return ChunkState::internal_error_;
                }
            }

            file_stream_->write(newBuffer->peek(), newBuffer->readable_bytes());
            if (!file_stream_->good()) {
                file_stream_->close();
                delete file_stream_;
                file_stream_ = nullptr;
                return ChunkState::internal_error_;
            }

            newBuffer->reset();
        }

        buffer::BufferedPool::get_instance()->free(buffer);

        return resultState;
    }

    bool ChunkedContentParser::parse(HttpPacket *packet)
    {
        ChunkState state = parse_chunked(packet);

        if (state == ChunkState::need_more_) {
            packet->set_body_state(BodyState::partial);
        }

        if (state == ChunkState::completed_) {
            Content *content = new Content(ContentType::chunked, nullptr);

            content->file_info_.tmp_file_name_ = rand_file_name_;
            content->file_info_.file_size_ = cur_chunk_size_;
            packet->set_body_content(content);
            packet->set_body_state(BodyState::fully);
        }

        return state == ChunkState::completed_;
    }
}