#ifndef __BUFFER_H__
#define __BUFFER_H__

#include <algorithm>
#include <cstddef>
#include <stdint.h>
#include <string>
#include <cstring>
#include <vector>

#include "../endian/endian.hpp"

// TODO fix 复用
class Buffer
{
public:
    int8_t read_uint8()
    {
        int8_t be8;
        ::memcpy(&be8, peek(), sizeof(int8_t));
        read_index += sizeof(int8_t);
        return be8;
    }

    uint8_t read_int8()
    {
        uint8_t ube8;
        ::memcpy(&ube8, peek(), sizeof(uint8_t));
        read_index += sizeof(uint8_t);
        return ube8;
    }

    int16_t read_uint16()
    {
        int16_t be16;
        ::memcpy(&be16, peek(), sizeof(int16_t));
        read_index += sizeof(int16_t);
        return endian::networkToHost16(be16);
    }

    uint16_t read_int16()
    {
        uint16_t ube16;
        ::memcpy(&ube16, peek(), sizeof(uint16_t));
        read_index += sizeof(uint16_t);
        return endian::networkToHost16(ube16);
    }

    uint32_t read_uint32()
    {
        uint32_t ube32;
        ::memcpy(&ube32, peek(), sizeof(uint32_t));
        read_index += sizeof(uint32_t);
        return endian::networkToHost32(ube32);
    }

    int32_t read_int32()
    {
        int32_t be32;
        ::memcpy(&be32, peek(), sizeof(int32_t));
        read_index += sizeof(int32_t);
        return endian::networkToHost32(be32);
    }

    uint64_t read_uint64()
    {
        uint64_t be64;
        ::memcpy(&be64, peek(), sizeof(uint64_t));
        read_index += sizeof(uint64_t);
        return endian::networkToHost64(be64);
    }

    int64_t read_int64()
    {
        int64_t be64;
        ::memcpy(&be64, peek(), sizeof(int64_t));
        read_index += sizeof(int64_t);
        return endian::networkToHost64(be64);
    }

public:
    void write_uint8(uint8_t val)
    {
        write((void *)&val, sizeof(uint8_t));
    }

    void write_int8(int8_t val)
    {
        write((void *)&val, sizeof(int8_t));
    }

    void write_uint16(uint16_t val)
    {
        uint16_t ube16 = endian::hostToNetwork16(val);
        write((void *)&ube16, sizeof(uint16_t));
    }

    void write_int16(int16_t val)
    {
        int16_t be16 = endian::hostToNetwork16(val);
        write((void *)&be16, sizeof(int16_t));
    }

    void write_uint32(uint32_t val)
    {
        uint32_t ube32 = endian::hostToNetwork32(val);
        write((void *)&ube32, sizeof(uint32_t));
    }

    void write_int32(int32_t val)
    {
        int32_t be32 = endian::hostToNetwork32(val);
        write((void *)&be32, sizeof(int32_t));
    }

    void write_uint64(uint64_t val)
    {
        uint64_t ube64 = endian::hostToNetwork64(val);
        write((void *)&ube64, sizeof(uint64_t));
    }

    void write_int64(int64_t val)
    {
        int64_t be64 = endian::hostToNetwork64(val);
        write((void *)&be64, sizeof(int64_t));
    }

    void write_bool(bool val)
    {
        write_int8(val ? 1 : 0);
    }

    void write_string(const std::string &str)
    {
        write(str.c_str(), str.size());
    }

    void write_string(const char *str, std::size_t length = 0)
    {
        size_t len = length ? length : strlen(str);
        write(str, len);
    }
    
public:
    char* begin()
    { 
        return &*buffs.begin();
    }

    const char* begin() const
    { 
        return &*buffs.begin();
    }

    char * buffer_begin()
    { 
        return &*(buffs.begin() + write_index);
    }

    const char* peek() const
    { 
        return begin() + read_index;
    }

    const char* peek_end() const
    { 
        return begin() + read_index + readable_bytes();
    }

    char * peek_for()
    { 
        return begin() + read_index;
    }

    size_t readable_bytes() const
    {
        return write_index - read_index;
    }

    size_t get_read_index() const
    {
        return read_index;
    }

    size_t get_write_index() const
    {
        return write_index;
    }

    void reset_read_index(const size_t idx)
    {
        if (idx >= buffs.size()) {
            return;
        }

        read_index = idx;
    }

    void reset()
    {
        read_index = 0;
        write_index = 0;
    }

    void set_write_index(size_t idx)
    {
        write_index = idx;
    }

    void set_read_index(size_t idx)
    {
        read_index = idx;
    }

    void add_read_index(size_t idx)
    {
        read_index += idx;
    }

    size_t writable_size() const
    {
        return buffs.size() - write_index;
    }

    void fill(size_t bytes)
    {
        write_index += bytes;
    }

    void resize(const size_t size = 1024)
    {
        std::vector<char> newBuffs(size);
        buffs.swap(newBuffs);
    }

    std::size_t get_buff_size() const
    {
        return buffs.size();
    }

    void append_buffer(const Buffer &buff)
    {
        if (buff.readable_bytes() > writable_size()) {
            std::vector<char> newBuffs(get_buff_size() - writable_size() + buff.readable_bytes());
            std::memcpy(&*newBuffs.begin(), begin(), readable_bytes());
            buffs.swap(newBuffs);
        }

        std::memcpy(buffer_begin(), buff.peek(), buff.readable_bytes());
        fill(buff.readable_bytes());
    }

    bool empty()
    {
        return readable_bytes() == 0;
    }

private:
    void write(const void * data, const size_t len)
    {
        const auto d = static_cast<const char*>(data);
        std::copy_n(d, len, begin() + write_index);
        write_index += len;
    }

    void write(const char * data, size_t len)
    {
        if (buffs.size() < len) {
            buffs.reserve(len);
        }

        std::copy(data, data + len, begin() + write_index);
        write_index += len;
    }

public:
    explicit Buffer(const std::size_t size = 0) : read_index(0), write_index(0)
    {
        if (size == 0) {
            buffs.resize(8192);
        } else {
            buffs.resize(size);
        }
    }

private:
    size_t read_index;
    size_t write_index;
    std::vector<char> buffs;
};

#endif // __BUFFER_H__