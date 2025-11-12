#ifndef __BUFFER_READER_H__
#define __BUFFER_READER_H__

#include "buffer.h"
#include "buffer/pool.h"
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

namespace yuan::buffer {

class BufferReader 
{
public:
    BufferReader()
    {
        init();
    }
    
    BufferReader(const std::vector<Buffer *> &buffers)
    {
        init();
        add_buffer(buffers);
    }

    BufferReader(Buffer *buffer)
    {
        init();
        add_buffer(buffer);
    }
    
    BufferReader(const BufferReader &) = delete;
    BufferReader &operator=(const BufferReader &) = delete;
    
    BufferReader(BufferReader &&) = delete;
    BufferReader &operator=(BufferReader &&) = delete;
    
    ~BufferReader() 
    {
        free_buffers();
    }

    void add_buffer(Buffer *buffer)
    {
        buffers_.push_back(buffer);
        total_bytes_ += buffer->readable_bytes();
    }

    void add_buffer(const std::vector<Buffer *> &buffers)
    {
        for (auto buffer : buffers) {
            add_buffer(buffer);
        }
    }

public:
    void init()
    {
        current_buf_idx_ = 0;
        current_offset_ = 0;
        total_bytes_ = 0;
        mark_buf_idx_ = 0;
        mark_offset_ = 0;
        cur_buf_idx_ = 0;
        free_buffers();
    }

    void mark()
    {
        mark_buf_idx_ = current_buf_idx_;
        mark_offset_ = current_offset_;
    }

    void rollback()
    {
        current_buf_idx_ = mark_buf_idx_;
        current_offset_ = mark_offset_;
    }

    void free_buffers()
    {
        for (auto buffer : buffers_) {
            BufferedPool::get_instance()->free(buffer);
        }
        buffers_.clear();
    }

public:
    // 0: 读取一行成功，-1: 没有读取到一行，-2: 读取到一行，但是没有换行符
    int read_line(std::string &line)
    {
        bool is_read_line = false;
        while (get_remain_bytes() > 0) {
            char ch = peek_char();
            if (ch == '\n') {
                read_char();
                is_read_line = true;
                break;
            } else if(ch == '\r') {
                read_char();
                if (peek_char() == '\n') {
                    read_char();
                    is_read_line = true;
                    break;
                } else {
                    return -2;
                }
            } else {
                line.push_back(ch);
                read_char();
            }
        }
        
        return is_read_line ? 0 : -1;
    }

    int read(char *ptr, size_t size)
    {
        if (size > get_remain_bytes()) {
            size = get_remain_bytes();
        }
        
        if (size == 0) {
            return 0;
        }

        int count = 0;
        char *p = ptr;
        while (count < size) {
            if (current_buf_idx_ >= buffers_.size()) {
                break;
            }
            
            *p++ = read_char();
            ++count;
        }

        return count;
    }

    bool read_match(const char *str)
    {
        if (str == nullptr) {
            return false;
        }

        if (strlen(str) > get_remain_bytes()) {
            return false;
        }

        if (strlen(str) == 0) {
            return true;
        }

        mark();

        const char *p = str;
        while (*p != '\0') {
            if (read_char() != *p) {
                break;
            }
            ++p;
        }

        if (*p == '\0') {
            return true;
        }

        rollback();

        return false;
    }

    bool read_match_ignore_case(const char *str)
    {
        if (str == nullptr) {
            return false;
        }

        if (strlen(str) > get_remain_bytes()) {
            return false;
        }

        if (strlen(str) == 0) {
            return true;
        }

        mark();

        const char *p = str;
        while (*p != '\0') {
            if (std::tolower(read_char()) != std::tolower(*p)) {
                break;
            }
            ++p;
        }

        if (*p == '\0') {
            return true;
        }

        rollback();
        
        return false;
    }

    char peek_char() const
    {
        if (current_buf_idx_ >= buffers_.size()) {
            return 0;
        }

        return buffers_[current_buf_idx_]->peek(cur_buf_idx_);
    }

    std::size_t get_remain_bytes() const
    {
        return total_bytes_ - current_offset_;
    }

    char read_char()
    {
        char ch = peek_char();
        inc_offset(1);
        return ch;
    }

    char operator *()
    {
        return peek_char();
    }

    void operator++()
    {
        if (current_offset_ >= total_bytes_) {
            return;
        }
        
        if (current_buf_idx_ >= buffers_.size()) {
            return;
        }

        inc_offset(1);
    }

    void operator++(int inc)
    {
        if (inc == 0) {
            return;
        }

        if (inc + current_offset_ > total_bytes_) {
            inc = total_bytes_ - current_offset_;
        }

        inc_offset(inc);
    }

    void operator--()
    {
        if (current_offset_ == 0) {
            return;
        }

        if (buffers_.empty()) {
            return;
        }

        --current_offset_;
        if (buffers_[current_buf_idx_]->readable_bytes() == 1) {
            --current_buf_idx_;
        }
    }

    void operator--(int desc)
    {
        if (desc == 0) {
            return;
        }

        if (desc > current_offset_) {
            desc = current_offset_;
        }

        current_offset_ -= desc;
        int count = desc;
        while (count >= buffers_[current_buf_idx_]->readable_bytes()) {
            count -= buffers_[current_buf_idx_]->readable_bytes();
            --current_buf_idx_;
        }
    }

    uint64_t write(std::ofstream &out)
    {
        if (!out.is_open() || !out.good()) {
            return -1;
        }

        if (buffers_.empty() || get_remain_bytes() == 0) {
            return 0;
        }

        uint64_t written = 0;
        
        while (cur_buf_idx_ < buffers_.size()) {
            out.write(buffers_[current_buf_idx_]->peek_for(), buffers_[current_buf_idx_]->readable_bytes());
            if (out.fail()) {
                return -1;
            }

            written += buffers_[current_buf_idx_]->readable_bytes();
            ++current_buf_idx_;
            current_offset_ += buffers_[current_buf_idx_]->readable_bytes();
        }

        return written;
    }

    uint64_t write_fail_rollback(std::ofstream &out)
    {
        if (!out.is_open() || !out.good()) {
            return -1;
        }

        if (buffers_.empty() || get_remain_bytes() == 0) {
            return 0;
        }

        mark();

        uint64_t written = 0;
        
        while (cur_buf_idx_ < buffers_.size()) {
            out.write(buffers_[current_buf_idx_]->peek_for(), buffers_[current_buf_idx_]->readable_bytes());
            if (out.fail()) {
                rollback();
                return -1;
            }

            written += buffers_[current_buf_idx_]->readable_bytes();
            ++current_buf_idx_;
            current_offset_ += buffers_[current_buf_idx_]->readable_bytes();
        }

        return written;
    }

private:
    void inc_offset(int inc)
    {
        if (inc == 0) {
            return;
        }

        if (inc + current_offset_ > total_bytes_) {
            inc = total_bytes_ - current_offset_;
        }

        current_offset_ += inc;
        int count = inc;
        int fromIdx = current_buf_idx_;
        int leftBytes = buffers_[current_buf_idx_]->readable_bytes() - cur_buf_idx_;
        while (count >= leftBytes) {
            count -= leftBytes;
            ++current_buf_idx_;
            if (current_buf_idx_ >= buffers_.size()) {
                break;
            }
            leftBytes = buffers_[current_buf_idx_]->readable_bytes();
        }

        if (fromIdx != current_buf_idx_) {
            cur_buf_idx_ = count;
        } else {
            cur_buf_idx_ += count;
        }
    }

private:
    size_t current_buf_idx_ = 0;
    size_t current_offset_ = 0;
    size_t total_bytes_ = 0;
    size_t mark_buf_idx_ = 0;
    size_t mark_offset_ = 0;
    size_t cur_buf_idx_ = 0;
    std::vector<Buffer *> buffers_;
};

} // namespace yuan::buffer

#endif // __BUFFER_READER_H__
