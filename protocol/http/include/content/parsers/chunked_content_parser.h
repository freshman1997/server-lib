#ifndef __CHUNKED_CONTENT_PARSER_H__
#define __CHUNKED_CONTENT_PARSER_H__
#include "buffer/buffer.h"
#include "content/content_parser.h"
#include <fstream>

namespace yuan::net::http 
{
    enum class ChunkState
    {
        completed_,
        need_more_,
        invalid_chunck_,
        internal_error_,
    };
    
    class ChunkedContentParser final : public ContentParser
    {
    public:
        ChunkedContentParser();
        ~ChunkedContentParser();
        
    public:
        virtual void reset() override;

        virtual std::size_t get_content_length() override;

        virtual bool can_parse(ContentType contentType) override;

        // 解析
        virtual bool parse(HttpPacket *packet) override;

    public:
        void set_exceed_save_file(bool flag)
        {
            exceed_save_file_ = flag;
        }

        void set_max_chunk_size(int size)
        {
            max_chunk_size_ = size;
        }

        void set_exceed_chunk_size_save_file(int size)
        {
            exceed_chunk_size_save_file_ = size;
        }

    private:
        ChunkState parse_chunked(HttpPacket *packet);

    private:
        bool exceed_save_file_;
        int max_chunk_size_;
        int exceed_chunk_size_save_file_;
        int cur_chunk_size_;
        std::string rand_file_name_;
        std::fstream *file_stream_;
        buffer::Buffer *cached_buffer_;
    };
}


#endif // __CHUNKED_CONTENT_PARSER_H__