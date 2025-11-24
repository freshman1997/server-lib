#ifndef __BUFFER_READER_H__
#define __BUFFER_READER_H__

#include "buffer.h"
#include "buffer/pool.h"
#include <cassert>
#include <cstddef>
#include <fstream>
#include <string>
#include <sstream>
#include <vector>
#include <iostream>

namespace yuan::buffer {

    class BufferReader
    {
    public:
        class Iterator
        {
        private:
            BufferReader *reader_ = nullptr;

        public:
            using iterator_concept = std::contiguous_iterator_tag;
            using iterator_category = std::contiguous_iterator_tag;
            using value_type = char;
            using difference_type = std::ptrdiff_t;
            using pointer = char *;
            using reference = char &;

        public:
            explicit Iterator(BufferReader *reader) : reader_(reader)
            {
                if (reader_) {
                    reader_->mark();
                }
            }

            ~Iterator()
            {
                if (reader_) {
                    reader_->rollback();
                }
            }

            value_type operator*() const { return reader_->peek_char(); }
            pointer operator->() const { return reader_->peek_p(); }

            Iterator& operator++() { ++*reader_; return *this; }
            Iterator& operator--() { --*reader_; return *this; }
            Iterator& operator+=(const difference_type n) { *reader_ += n; return *this; }
            Iterator& operator-=(const difference_type n) { *reader_ -= n; return *this; }

            value_type operator[](const difference_type n) const { return *reader_[n]; }

            friend bool operator==(const Iterator& a, const Iterator& b)
            {
                if (!a.reader_) return false;
                return a.reader_->readable_bytes() == 0;
            }

            friend Iterator operator+(Iterator it, difference_type n) {
                return it += n;
            }

            friend Iterator operator+(const difference_type n, Iterator it) {
                return it += n;
            }

            friend Iterator operator-(Iterator it, const difference_type n) {
                return it -= n;
            }

            friend difference_type operator-(const Iterator& a, const Iterator& b) {
                return a.reader_ - b.reader_;
            }
        };

    public: // it
        Iterator begin()
        {
            return Iterator(this);
        }

        Iterator end()
        {
            return Iterator(nullptr);
        }

    public:
        BufferReader()
        {
            init();
        }

        explicit BufferReader(const std::vector<Buffer *> &buffers)
        {
            init();
            add_buffer(buffers);
        }

        explicit BufferReader(Buffer *buffer)
        {
            init();
            add_buffer(buffer);
        }

        BufferReader(const BufferReader &other) = delete;
        BufferReader &operator=(const BufferReader &other) = delete;

        BufferReader(BufferReader &&other) noexcept
        {
            other.copy_to_and_clean(*this);
        }

        BufferReader & operator=(BufferReader &&other) noexcept
        {
            copy_to_and_clean(other);
            return *this;
        }

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
            for (const auto buffer : buffers) {
                add_buffer(buffer);
            }
        }

        void copy_to_and_clean(BufferReader &other)
        {
            const auto &buffers = take_buffers();
            other.add_buffer(buffers);
        }

    public:
        void init()
        {
            reset_idx();
            free_buffers();
        }

        void mark()
        {
            mark_buf_idx_ = current_buffers_idx_;
            mark_offset_ = current_offset_;
            mark_cur_buf_idx_ = cur_buf_idx_;
        }

        void rollback()
        {
            current_buffers_idx_ = mark_buf_idx_;
            current_offset_ = mark_offset_;
            cur_buf_idx_ = mark_cur_buf_idx_;
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
                if (const char ch = peek_char(); ch == '\n') {
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
                if (current_buffers_idx_ >= buffers_.size()) {
                    break;
                }

                *p++ = read_char();
                ++count;
            }

            return count;
        }

        int read_some(char *ptr, const size_t begin, size_t size) const
        {
            if (size > get_remain_bytes()) {
                size = get_remain_bytes();
            }

            if (size == 0) {
                return 0;
            }

            int count = 0;
            char *p = ptr;
            size_t idx = 0, bytes = 0;
            for (; idx < buffers_.size(); idx++) {
                if (buffers_[idx]->readable_bytes() + bytes >= begin) {
                    break;
                }
                bytes += buffers_[idx]->readable_bytes();
            }

            size_t offset = buffers_[idx]->readable_bytes() - (buffers_[idx]->readable_bytes() + bytes - begin);
            const char *buffBegin = buffers_[idx]->peek() + offset;
            while (count < size) {
                if (idx >= buffers_.size()) {
                    break;
                }

                *p++ = *buffBegin++;
                ++count;
                ++offset;

                if (offset >= buffers_[idx]->readable_bytes()) {
                    ++idx;
                    offset = 0;
                }
            }

            return count;
        }

        bool read_match(const char *str, size_t sz = 0)
        {
            if (str == nullptr) {
                return false;
            }

            if (sz == 0) {
                sz = strlen(str);
            }

            if (sz == 0) {
                return true;
            }

            if (sz > get_remain_bytes()) {
                return false;
            }

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

            return false;
        }

        bool read_match_ignore_case(const char *str)
        {
            if (str == nullptr) {
                return false;
            }

            const size_t len = strlen(str);
            if (len == 0) {
                return true;
            }

            if (len > get_remain_bytes()) {
                return false;
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

        int find_match(const std::string &pattern, const bool rb = false, const bool ignoreCase = false)
        {
            if (pattern.empty()) {
                return 0;
            }

            if (readable_bytes() < pattern.size()) {
                return -1;
            }

            if (rb) mark();

            const size_t from = get_read_offset();
            const size_t res = search_next(*this, pattern, ignoreCase);
            if (res == std::string::npos) {
                if (rb) rollback();
                return -1;
            }

            const int len = res - from - pattern.size();

            if (rb) rollback();

            if (res < pattern.size()) {
                return -1;
            }

            if (!rb) inc_offset(res - from);

            return len;
        }

        int find_match_ignore_case(const std::string &pattern, const bool rb = false)
        {
            return find_match(pattern, rb, true);
        }

        char peek_char() const
        {
            if (current_buffers_idx_ >= buffers_.size()) {
                return 0;
            }

            return buffers_[current_buffers_idx_]->peek(cur_buf_idx_);
        }

        char & peek_r() const
        {
            return *buffers_[current_buffers_idx_]->peek_for();
        }

        char * peek_p() const
        {
            return buffers_[current_buffers_idx_]->peek_for();
        }

        std::size_t get_remain_bytes() const
        {
            return total_bytes_ - current_offset_;
        }

        std::size_t readable_bytes() const
        {
            return get_remain_bytes();
        }

        std::size_t get_total_bytes() const
        {
            return total_bytes_;
        }

        char read_char()
        {
            const char ch = peek_char();
            inc_offset(1);
            return ch;
        }

        char read_int8()
        {
            return read_char();
        }

        bool skip_newline_symbol()
        {
            if (readable_bytes() == 0) {
                return false;
            }

            if (peek_char() == '\n') {
                read_char();
                return true;
            }

            return read_match("\r\n");
        }

        std::size_t get_read_offset() const
        {
            return current_offset_;
        }

        explicit operator bool() const
        {
            return readable_bytes() > 0;
        }

        char operator *() const
        {
            return peek_char();
        }

        BufferReader & operator++()
        {
            if (current_offset_ >= total_bytes_) {
                return *this;
            }

            if (current_buffers_idx_ >= buffers_.size()) {
                return *this;
            }

            inc_offset(1);

            return *this;
        }

        BufferReader & operator++(int inc)
        {
            if (inc == 0) {
                return *this;
            }

            if (inc + current_offset_ > total_bytes_) {
                inc = total_bytes_ - current_offset_;
            }

            inc_offset(inc);

            return *this;
        }

        BufferReader & operator+=(size_t inc)
        {
            if (inc == 0) {
                return *this;
            }

            if (inc + current_offset_ > total_bytes_) {
                inc = total_bytes_ - current_offset_;
            }

            inc_offset(inc);

            return *this;
        }

        BufferReader & operator--()
        {
            if (current_offset_ == 0) {
                return *this;
            }

            if (buffers_.empty()) {
                return *this;
            }

            --current_offset_;
            --cur_buf_idx_;
            if (buffers_[current_buffers_idx_]->readable_bytes() == 1) {
                --current_buffers_idx_;
            }

            return *this;
        }

        BufferReader & operator-=(size_t desc)
        {
            if (desc == 0) {
                return *this;
            }

            if (desc > current_offset_) {
                desc = current_offset_;
            }

            size_t idx = 0, bytes = 0;
            for (; idx < buffers_.size(); idx++) {
                bytes += buffers_[idx]->readable_bytes();
                if (buffers_[idx]->readable_bytes() + bytes >= current_offset_) {
                    break;
                }
            }

            if (const size_t off = bytes - current_offset_; desc > off) {
                --current_buffers_idx_;
                desc -= off;
            }

            size_t count = desc;
            while (count >= buffers_[current_buffers_idx_]->readable_bytes()) {
                count -= buffers_[current_buffers_idx_]->readable_bytes();
                --current_buffers_idx_;
                cur_buf_idx_ = 0;
            }

            if (count > 0) {
                cur_buf_idx_ -= count;
            }

            current_offset_ -= desc;

            return *this;
        }

        unsigned char operator[](const size_t index) const
        {
            return index_char(index);
        }

        unsigned char index_char(const size_t index) const
        {
            size_t idx = 0, bytes = 0;
            for (; idx < buffers_.size(); idx++) {
                if (buffers_[idx]->readable_bytes() + bytes >= index) {
                    break;
                }
                bytes += buffers_[idx]->readable_bytes();
            }

            idx = std::min(idx, buffers_.size() - 1);
            const size_t offset = buffers_[idx]->readable_bytes() - (buffers_[idx]->readable_bytes() + bytes - index);
            return *(buffers_[idx]->begin() + offset);
        }

        int64_t write(std::ofstream &out, int64_t sz = -1)
        {
            if (!out.is_open() || !out.good()) {
                return -1;
            }

            if (buffers_.empty() || get_remain_bytes() == 0) {
                return 0;
            }

            int64_t written = 0;

            if (sz < 0) {
                sz = get_remain_bytes();
            }

            while (current_buffers_idx_ < buffers_.size() && sz > 0) {
                const int64_t bytes = sz > buffers_[current_buffers_idx_]->readable_bytes() ? buffers_[current_buffers_idx_]->readable_bytes() : sz;
                out.write(buffers_[current_buffers_idx_]->peek_for(), buffers_[current_buffers_idx_]->readable_bytes());
                if (out.fail()) return -1;
                sz -= bytes;
                written += bytes;
                ++current_buffers_idx_;
                current_offset_ += bytes;
            }

            return written;
        }

        int64_t write_from_offset(std::ofstream &out, const size_t offset, int64_t sz = -1) const
        {
            if (!out.is_open() || !out.good()) {
                return -1;
            }

            if (buffers_.empty() || get_remain_bytes() == 0) {
                return 0;
            }

            int64_t written = 0;

            size_t idx = 0, bytes = 0;
            for (; idx < buffers_.size(); idx++) {
                if (buffers_[idx]->readable_bytes() + bytes >= offset) {
                    break;
                }
                bytes += buffers_[idx]->readable_bytes();
            }

            if (sz < 0) {
                sz = total_bytes_ - offset;
            }

            size_t pos = buffers_[idx]->readable_bytes() - (buffers_[idx]->readable_bytes() + bytes - offset);
            while (idx < buffers_.size() && sz > 0) {
                const int64_t writeBytes = sz > buffers_[idx]->readable_bytes() - pos ? buffers_[idx]->readable_bytes() - pos : sz;
                out.write(buffers_[idx]->peek_for() + pos, writeBytes);
                if (out.fail()) return -1;
                sz -= writeBytes;
                written += writeBytes;
                ++idx;
                pos = 0;
            }

            return written;
        }

        std::string read_from_offset(const size_t offset, int64_t sz, bool ignoreCase = false) const
        {
            int64_t written = 0;

            size_t idx = 0, bytes = 0;
            for (; idx < buffers_.size(); idx++) {
                if (buffers_[idx]->readable_bytes() + bytes >= offset) {
                    break;
                }
                bytes += buffers_[idx]->readable_bytes();
            }

            if (sz < 0) {
                sz = total_bytes_ - offset;
            }

            std::stringstream ss;
            size_t pos = buffers_[idx]->readable_bytes() - (buffers_[idx]->readable_bytes() + bytes - offset);
            while (idx < buffers_.size() && sz > 0) {
                const int64_t writeBytes = sz > buffers_[idx]->readable_bytes() - pos ? buffers_[idx]->readable_bytes() - pos : sz;
                ss << std::string(buffers_[idx]->peek_for() + pos, writeBytes);
                sz -= writeBytes;
                written += writeBytes;
                ++idx;
                pos = 0;
            }

            if (!ignoreCase) {
                return ss.str();
            }

            std::string res = ss.str();
            for (char & re : res) {
                re = std::tolower(re);
            }

            return res;
        }

        int64_t write_fail_rollback(std::ofstream &out)
        {
            if (!out.is_open() || !out.good()) {
                return -1;
            }

            if (buffers_.empty() || get_remain_bytes() == 0) {
                return 0;
            }

            mark();

            int64_t written = 0;
            while (cur_buf_idx_ < buffers_.size()) {
                out.write(buffers_[current_buffers_idx_]->peek_for(), buffers_[current_buffers_idx_]->readable_bytes());
                if (out.fail()) {
                    rollback();
                    return -1;
                }

                written += buffers_[current_buffers_idx_]->readable_bytes();
                ++current_buffers_idx_;
                current_offset_ += buffers_[current_buffers_idx_]->readable_bytes();
            }

            return written;
        }

        void clear()
        {
            if (buffers_.empty()) {
                return;
            }

            init();
        }

        void reset_buffer_index()
        {
            reset_idx();
            for (const auto & buffer : buffers_) {
                buffer->reset();
            }
        }

        size_t writable_size() const
        {
            return get_writable_bytes();
        }

        size_t get_writable_bytes() const
        {
            size_t totalBytes = 0;
            for (const auto & buffer : buffers_) {
                totalBytes += buffer->writable_size();
            }
            return totalBytes;
        }

        int64_t fill_from_stream(std::ifstream &istream, const size_t size) const
        {
            if (!istream.is_open() || !istream.good()) {
                return -1;
            }

            if (get_writable_bytes() < size) {
                return -2;
            }

            int64_t realReadBytes = 0;
            istream.clear();
            for (const auto & buffer : buffers_) {
                if (buffer->writable_size() > 0) {
                    istream.read(buffer->peek_for(), buffer->writable_size());
                    if (const size_t read = istream.gcount(); read < buffer->writable_size()) {
                        return -3;
                    }
                    realReadBytes += buffer->writable_size();
                    buffer->fill(buffer->writable_size());
                }
            }

            return realReadBytes;
        }

        std::vector<Buffer *> take_buffers()
        {
            std::vector<Buffer *> tmpBuffers;
            tmpBuffers.reserve(buffers_.size());
            for (const auto & buffer : buffers_) {
                tmpBuffers.push_back(buffer);
            }

            reset_idx();
            buffers_.clear();

            return tmpBuffers;
        }

    private:
        void inc_offset(size_t inc)
        {
            if (inc == 0) {
                return;
            }

            if (inc + current_offset_ > total_bytes_) {
                inc = static_cast<int>(total_bytes_ - current_offset_);
            }

            current_offset_ += inc;
            size_t count = inc;
            const size_t fromIdx = current_buffers_idx_;
            size_t leftBytes = buffers_[current_buffers_idx_]->readable_bytes() - cur_buf_idx_;
            while (count >= leftBytes) {
                count -= leftBytes;
                ++current_buffers_idx_;
                if (current_buffers_idx_ >= buffers_.size()) {
                    break;
                }
                leftBytes = buffers_[current_buffers_idx_]->readable_bytes();
            }

            if (fromIdx != current_buffers_idx_) {
                cur_buf_idx_ = count;
            } else {
                cur_buf_idx_ += count;
            }
        }

        void reset_idx()
        {
            current_buffers_idx_ = 0;
            current_offset_ = 0;
            total_bytes_ = 0;
            mark_buf_idx_ = 0;
            mark_offset_ = 0;
            cur_buf_idx_ = 0;
        }

        static std::vector<int> build_lps(const std::string& pattern) {
            int m = pattern.length();
            std::vector<int> lps(m, 0);

            int len = 0;  // 当前最长匹配前缀后缀的长度
            int i = 1;    // 从pattern[1]开始，因为lps[0]总是0

            while (i < m) {
                if (pattern[i] == pattern[len]) {
                    len++;
                    lps[i] = len;
                    i++;
                } else {
                    if (len != 0) {
                        // 回退，但不增加i
                        len = lps[len - 1];
                    } else {
                        // len为0，无法再回退
                        lps[i] = 0;
                        i++;
                    }
                }
            }

            return lps;
        }

        static size_t search_next(const BufferReader &reader, const std::string& pattern, const bool ignoreCase)
        {
            std::vector<int> positions;
            const size_t n = reader.get_total_bytes();
            const size_t m = pattern.length();

            if (m == 0) return std::string::npos;

            const std::vector<int> lps = build_lps(pattern);

            size_t i = reader.get_read_offset();  // text的索引
            size_t j = 0;  // pattern的索引

            while (i < n) {
                unsigned char ch = ignoreCase ? std::tolower(reader.index_char(i)) : reader.index_char(i);
                unsigned char ch1 = ignoreCase ? std::tolower(pattern[j]) : pattern[j];
                if (ch1 == ch) {
                    i++;
                    j++;
                }

                if (j == m) {
                    return i;
                }

                ch = ignoreCase ? std::tolower(reader.index_char(i)) : reader.index_char(i);
                ch1 = ignoreCase ? std::tolower(pattern[j]) : pattern[j];
                if (i < n && ch != ch1) {
                    if (j != 0) {
                        j = lps[j - 1];
                    } else {
                        i++;
                    }
                }
            }
            return std::string::npos;
        }

    private:
        size_t current_buffers_idx_ = 0;
        size_t current_offset_ = 0;
        size_t total_bytes_ = 0;
        size_t cur_buf_idx_ = 0;
        size_t mark_buf_idx_ = 0;
        size_t mark_offset_ = 0;
        size_t mark_cur_buf_idx_ = 0;
        std::vector<Buffer *> buffers_;
    };
} // namespace yuan::buffer

#endif // __BUFFER_READER_H__
