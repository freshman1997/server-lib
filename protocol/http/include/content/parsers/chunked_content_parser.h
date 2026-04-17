#ifndef __CHUNKED_CONTENT_PARSER_H__
#define __CHUNKED_CONTENT_PARSER_H__

#include "buffer/byte_buffer.h"
#include "content/content_parser.h"

#include <fstream>
#include <memory>
#include <string>

namespace yuan::net::http
{
    enum class ChunkState {
        completed_,
        need_more_,
        invalid_chunk_,
        internal_error_,
    };

    class ChunkedContentParser final : public ContentParser
    {
    public:
        ChunkedContentParser();
        ~ChunkedContentParser() override;

        void reset() override;
        std::size_t get_content_length() override;
        bool can_parse(ContentType contentType) override;
        bool parse(HttpPacket *packet) override;

        void set_exceed_save_file(bool flag)
        {
            exceed_save_file_ = flag;
        }
        void set_max_chunk_size(std::size_t size)
        {
            max_chunk_size_ = size;
        }
        void set_disk_spill_size(std::size_t size)
        {
            disk_spill_size_ = size;
        }

    private:
        enum class ParsePhase {
            read_size,
            read_data,
            read_data_crlf,
            read_trailer,
            done,
            error,
        };

        ChunkState parse_chunked(HttpPacket *packet);
        ChunkState feed_data();

    private:
        bool exceed_save_file_ = true;
        std::size_t max_chunk_size_ = 1024 * 1024 * 1024;
        std::size_t disk_spill_size_ = 1 * 1024 * 1024;
        static constexpr std::size_t max_size_buf_length_ = 16;
        ParsePhase phase_ = ParsePhase::read_size;
        std::size_t total_bytes_ = 0;
        std::size_t remain_bytes_ = 0;
        std::string size_buf_;
        ::yuan::buffer::ByteBuffer pending_buffer_;
        ::yuan::buffer::ByteBuffer mem_buffer_;
        std::unique_ptr<std::fstream> file_stream_;
        std::string tmp_file_path_;
        std::string trailer_checksum_;
    };
}

#endif
